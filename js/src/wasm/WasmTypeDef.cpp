/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2015 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmTypeDef.h"

#include "mozilla/MathAlgorithms.h"

#include "jit/JitOptions.h"
#include "js/friend/ErrorMessages.h"  // JSMSG_*
#include "js/Printf.h"
#include "js/Value.h"
#include "vm/StringType.h"
#include "wasm/WasmJS.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

using mozilla::IsPowerOfTwo;

using ImmediateType = uint32_t;  // for 32/64 consistency
static const unsigned sTotalBits = sizeof(ImmediateType) * 8;
static const unsigned sTagBits = 1;
static const unsigned sReturnBit = 1;
static const unsigned sLengthBits = 4;
static const unsigned sTypeBits = 3;
static const unsigned sMaxTypes =
    (sTotalBits - sTagBits - sReturnBit - sLengthBits) / sTypeBits;

static bool IsImmediateValType(ValType vt) {
  switch (vt.kind()) {
    case ValType::I32:
    case ValType::I64:
    case ValType::F32:
    case ValType::F64:
    case ValType::V128:
      return true;
    case ValType::Ref:
      switch (vt.refTypeKind()) {
        case RefType::Func:
        case RefType::Extern:
        case RefType::Eq:
          return true;
        case RefType::TypeRef:
          return false;
      }
      break;
  }
  MOZ_CRASH("bad ValType");
}

static unsigned EncodeImmediateValType(ValType vt) {
  static_assert(7 < (1 << sTypeBits), "fits");
  switch (vt.kind()) {
    case ValType::I32:
      return 0;
    case ValType::I64:
      return 1;
    case ValType::F32:
      return 2;
    case ValType::F64:
      return 3;
    case ValType::V128:
      return 4;
    case ValType::Ref:
      switch (vt.refTypeKind()) {
        case RefType::Func:
          return 5;
        case RefType::Extern:
          return 6;
        case RefType::Eq:
          return 7;
        case RefType::TypeRef:
          break;
      }
      break;
  }
  MOZ_CRASH("bad ValType");
}

static bool IsImmediateFuncType(const FuncType& funcType) {
  const ValTypeVector& results = funcType.results();
  const ValTypeVector& args = funcType.args();
  if (results.length() + args.length() > sMaxTypes) {
    return false;
  }

  if (results.length() > 1) {
    return false;
  }

  for (ValType v : results) {
    if (!IsImmediateValType(v)) {
      return false;
    }
  }

  for (ValType v : args) {
    if (!IsImmediateValType(v)) {
      return false;
    }
  }

  return true;
}

static ImmediateType LengthToBits(uint32_t length) {
  static_assert(sMaxTypes <= ((1 << sLengthBits) - 1), "fits");
  MOZ_ASSERT(length <= sMaxTypes);
  return length;
}

static ImmediateType EncodeImmediateFuncType(const FuncType& funcType) {
  ImmediateType immediate = FuncType::ImmediateBit;
  uint32_t shift = sTagBits;

  if (funcType.results().length() > 0) {
    MOZ_ASSERT(funcType.results().length() == 1);
    immediate |= (1 << shift);
    shift += sReturnBit;

    immediate |= EncodeImmediateValType(funcType.results()[0]) << shift;
    shift += sTypeBits;
  } else {
    shift += sReturnBit;
  }

  immediate |= LengthToBits(funcType.args().length()) << shift;
  shift += sLengthBits;

  for (ValType argType : funcType.args()) {
    immediate |= EncodeImmediateValType(argType) << shift;
    shift += sTypeBits;
  }

  MOZ_ASSERT(shift <= sTotalBits);
  return immediate;
}

void FuncType::initImmediateTypeId() {
  if (!IsImmediateFuncType(*this)) {
    immediateTypeId_ = NO_IMMEDIATE_TYPE_ID;
    return;
  }
  immediateTypeId_ = EncodeImmediateFuncType(*this);
}

bool FuncType::canHaveJitEntry() const {
  return !hasUnexposableArgOrRet() &&
         !temporarilyUnsupportedReftypeForEntry() &&
         !temporarilyUnsupportedResultCountForJitEntry() &&
         JitOptions.enableWasmJitEntry;
}

bool FuncType::canHaveJitExit() const {
  return !hasUnexposableArgOrRet() && !temporarilyUnsupportedReftypeForExit() &&
         !hasInt64Arg() && !temporarilyUnsupportedResultCountForJitExit() &&
         JitOptions.enableWasmJitExit;
}

size_t FuncType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return args_.sizeOfExcludingThis(mallocSizeOf);
}

static inline CheckedInt32 RoundUpToAlignment(CheckedInt32 address,
                                              uint32_t align) {
  MOZ_ASSERT(IsPowerOfTwo(align));

  // Note: Be careful to order operators such that we first make the
  // value smaller and then larger, so that we don't get false
  // overflow errors due to (e.g.) adding `align` and then
  // subtracting `1` afterwards when merely adding `align-1` would
  // not have overflowed. Note that due to the nature of two's
  // complement representation, if `address` is already aligned,
  // then adding `align-1` cannot itself cause an overflow.

  return ((address + (align - 1)) / align) * align;
}

CheckedInt32 StructLayout::addField(FieldType type) {
  uint32_t fieldSize = type.size();
  uint32_t fieldAlignment = type.alignmentInStruct();

  // We have to ensure that `offset` is chosen so that no field crosses the
  // inline/outline boundary.  The assertions here ensure that.  See comment
  // on `class StructLayout` for background.
  MOZ_ASSERT(fieldSize >= 1 && fieldSize <= 16);
  MOZ_ASSERT((fieldSize & (fieldSize - 1)) == 0);  // is a power of 2
  MOZ_ASSERT(fieldAlignment == fieldSize);         // is naturally aligned

  // Alignment of the struct is the max of the alignment of its fields.
  structAlignment = std::max(structAlignment, fieldAlignment);

  // Align the pointer.
  CheckedInt32 offset = RoundUpToAlignment(sizeSoFar, fieldAlignment);
  if (!offset.isValid()) {
    return offset;
  }

  // Allocate space.
  sizeSoFar = offset + fieldSize;
  if (!sizeSoFar.isValid()) {
    return sizeSoFar;
  }

  // The following should hold if the three assertions above hold.
  MOZ_ASSERT(offset / 16 == (offset + fieldSize - 1) / 16);
  return offset;
}

CheckedInt32 StructLayout::close() {
  return RoundUpToAlignment(sizeSoFar, structAlignment);
}

bool StructType::init() {
  StructLayout layout;
  for (StructField& field : fields_) {
    CheckedInt32 offset = layout.addField(field.type);
    if (!offset.isValid()) {
      return false;
    }
    field.offset = offset.value();
  }

  CheckedInt32 size = layout.close();
  if (!size.isValid()) {
    return false;
  }
  size_ = size.value();

  return true;
}

size_t StructType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return fields_.sizeOfExcludingThis(mallocSizeOf);
}

size_t ArrayType::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  return 0;
}

size_t TypeDef::sizeOfExcludingThis(MallocSizeOf mallocSizeOf) const {
  switch (kind_) {
    case TypeDefKind::Struct: {
      return structType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::Func: {
      return funcType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::Array: {
      return arrayType_.sizeOfExcludingThis(mallocSizeOf);
    }
    case TypeDefKind::None: {
      return 0;
    }
    default:
      break;
  }
  MOZ_ASSERT_UNREACHABLE();
  return 0;
}

TypeResult TypeContext::isRefEquivalent(RefType first, RefType second,
                                        TypeCache* cache) const {
  // Anything's equal to itself.
  if (first == second) {
    return TypeResult::True;
  }

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  if (features_.functionReferences) {
    // References must have the same nullability to be equal
    if (first.isNullable() != second.isNullable()) {
      return TypeResult::False;
    }

    // Non type-index references are equal if they have the same kind
    if (!first.isTypeRef() && !second.isTypeRef() &&
        first.kind() == second.kind()) {
      return TypeResult::True;
    }

    // Type-index references can be equal
    if (first.isTypeRef() && second.isTypeRef()) {
      return isTypeDefEquivalent(first.typeDef(), second.typeDef(), cache);
    }
  }
#endif
  return TypeResult::False;
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
TypeResult TypeContext::isTypeDefEquivalent(const TypeDef* first,
                                            const TypeDef* second,
                                            TypeCache* cache) const {
  MOZ_ASSERT(features_.functionReferences);

  // Anything's equal to itself.
  if (first == second) {
    return TypeResult::True;
  }

#  ifdef ENABLE_WASM_GC
  if (features_.gc) {
    // A struct may be equal to a struct
    if (first->isStructType() && second->isStructType()) {
      return isStructEquivalent(first, second, cache);
    }

    // An array may be equal to an array
    if (first->isArrayType() && second->isArrayType()) {
      return isArrayEquivalent(first, second, cache);
    }
  }
#  endif

  return TypeResult::False;
}
#endif

#ifdef ENABLE_WASM_GC
TypeResult TypeContext::isStructEquivalent(const TypeDef* first,
                                           const TypeDef* second,
                                           TypeCache* cache) const {
  if (cache->isEquivalent(first, second)) {
    return TypeResult::True;
  }

  const StructType& firstStruct = first->structType();
  const StructType& secondStruct = second->structType();

  // Structs must have the same number of fields to be equal
  if (firstStruct.fields_.length() != secondStruct.fields_.length()) {
    return TypeResult::False;
  }

  // Assume these structs are equal while checking fields. If any field is
  // not equal then we remove the assumption.
  if (!cache->markEquivalent(first, second)) {
    return TypeResult::OOM;
  }

  for (uint32_t i = 0; i < secondStruct.fields_.length(); i++) {
    TypeResult result = isStructFieldEquivalent(firstStruct.fields_[i],
                                                secondStruct.fields_[i], cache);
    if (result != TypeResult::True) {
      cache->unmarkEquivalent(first, second);
      return result;
    }
  }
  return TypeResult::True;
}

TypeResult TypeContext::isStructFieldEquivalent(const StructField first,
                                                const StructField second,
                                                TypeCache* cache) const {
  // Struct fields must share the same mutability to equal
  if (first.isMutable != second.isMutable) {
    return TypeResult::False;
  }
  // Struct field types must be equal
  return isEquivalent(first.type, second.type, cache);
}

TypeResult TypeContext::isArrayEquivalent(const TypeDef* firstDef,
                                          const TypeDef* secondDef,
                                          TypeCache* cache) const {
  if (cache->isEquivalent(firstDef, secondDef)) {
    return TypeResult::True;
  }

  const ArrayType& firstArray = firstDef->arrayType();
  const ArrayType& secondArray = secondDef->arrayType();

  // Assume these arrays are equal while checking fields. If the array
  // element is not equal then we remove the assumption.
  if (!cache->markEquivalent(firstDef, secondDef)) {
    return TypeResult::OOM;
  }

  TypeResult result = isArrayElementEquivalent(firstArray, secondArray, cache);
  if (result != TypeResult::True) {
    cache->unmarkEquivalent(firstDef, secondDef);
  }
  return result;
}

TypeResult TypeContext::isArrayElementEquivalent(const ArrayType& first,
                                                 const ArrayType& second,
                                                 TypeCache* cache) const {
  // Array elements must share the same mutability to be equal
  if (first.isMutable_ != second.isMutable_) {
    return TypeResult::False;
  }
  // Array elements must be equal
  return isEquivalent(first.elementType_, second.elementType_, cache);
}
#endif

TypeResult TypeContext::isRefSubtypeOf(RefType subType, RefType superType,
                                       TypeCache* cache) const {
  // Anything's a subtype of itself.
  if (subType == superType) {
    return TypeResult::True;
  }

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
  if (features_.functionReferences) {
    // A subtype must have the same nullability as the supertype or the
    // supertype must be nullable.
    if (!(subType.isNullable() == superType.isNullable() ||
          superType.isNullable())) {
      return TypeResult::False;
    }

    // Non type-index references are subtypes if they have the same kind
    if (!subType.isTypeRef() && !superType.isTypeRef() &&
        subType.kind() == superType.kind()) {
      return TypeResult::True;
    }

    // Structs are subtypes of eqref
    if (subType.isTypeRef() && subType.typeDef()->isStructType() &&
        superType.isEq()) {
      return TypeResult::True;
    }

    // Arrays are subtypes of eqref
    if (subType.isTypeRef() && subType.typeDef()->isArrayType() &&
        superType.isEq()) {
      return TypeResult::True;
    }

    // Funcs are subtypes of funcref
    if (subType.isTypeRef() && subType.typeDef()->isFuncType() &&
        superType.isFunc()) {
      return TypeResult::True;
    }

    // Type-index references can be subtypes
    if (subType.isTypeRef() && superType.isTypeRef()) {
      return isTypeDefSubtypeOf(subType.typeDef(), superType.typeDef(), cache);
    }
  }
#endif
  return TypeResult::False;
}

#ifdef ENABLE_WASM_FUNCTION_REFERENCES
TypeResult TypeContext::isTypeDefSubtypeOf(const TypeDef* subType,
                                           const TypeDef* superType,
                                           TypeCache* cache) const {
  MOZ_ASSERT(features_.functionReferences);

  // Anything's a subtype of itself.
  if (subType == superType) {
    return TypeResult::True;
  }

#  ifdef ENABLE_WASM_GC
  if (features_.gc) {
    // Structs may be subtypes of structs
    if (subType->isStructType() && superType->isStructType()) {
      return isStructSubtypeOf(subType, superType, cache);
    }

    // Arrays may be subtypes of arrays
    if (subType->isArrayType() && superType->isArrayType()) {
      return isArraySubtypeOf(subType, superType, cache);
    }
  }
#  endif
  return TypeResult::False;
}
#endif

#ifdef ENABLE_WASM_GC
TypeResult TypeContext::isStructSubtypeOf(const TypeDef* subType,
                                          const TypeDef* superType,
                                          TypeCache* cache) const {
  if (cache->isSubtypeOf(subType, superType)) {
    return TypeResult::True;
  }

  const StructType& subStruct = subType->structType();
  const StructType& superStruct = superType->structType();

  // A subtype must have at least as many fields as its supertype
  if (subStruct.fields_.length() < superStruct.fields_.length()) {
    return TypeResult::False;
  }

  // Assume these structs are subtypes while checking fields. If any field
  // fails a check then we remove the assumption.
  if (!cache->markSubtypeOf(subType, superType)) {
    return TypeResult::OOM;
  }

  for (uint32_t i = 0; i < superStruct.fields_.length(); i++) {
    TypeResult result = isStructFieldSubtypeOf(subStruct.fields_[i],
                                               superStruct.fields_[i], cache);
    if (result != TypeResult::True) {
      cache->unmarkSubtypeOf(subType, superType);
      return result;
    }
  }
  return TypeResult::True;
}

TypeResult TypeContext::isStructFieldSubtypeOf(const StructField subType,
                                               const StructField superType,
                                               TypeCache* cache) const {
  // Mutable fields are invariant w.r.t. field types
  if (subType.isMutable && superType.isMutable) {
    return isEquivalent(subType.type, superType.type, cache);
  }
  // Immutable fields are covariant w.r.t. field types
  if (!subType.isMutable && !superType.isMutable) {
    return isSubtypeOf(subType.type, superType.type, cache);
  }
  return TypeResult::False;
}

TypeResult TypeContext::isArraySubtypeOf(const TypeDef* subType,
                                         const TypeDef* superType,
                                         TypeCache* cache) const {
  if (cache->isSubtypeOf(subType, superType)) {
    return TypeResult::True;
  }

  const ArrayType& subArray = subType->arrayType();
  const ArrayType& superArray = superType->arrayType();

  // Assume these arrays are subtypes while checking elements. If the elements
  // fail the check then we remove the assumption.
  if (!cache->markSubtypeOf(subType, superType)) {
    return TypeResult::OOM;
  }

  TypeResult result = isArrayElementSubtypeOf(subArray, superArray, cache);
  if (result != TypeResult::True) {
    cache->unmarkSubtypeOf(subType, superType);
  }
  return result;
}

TypeResult TypeContext::isArrayElementSubtypeOf(const ArrayType& subType,
                                                const ArrayType& superType,
                                                TypeCache* cache) const {
  // Mutable elements are invariant w.r.t. field types
  if (subType.isMutable_ && superType.isMutable_) {
    return isEquivalent(subType.elementType_, superType.elementType_, cache);
  }
  // Immutable elements are covariant w.r.t. field types
  if (!subType.isMutable_ && !superType.isMutable_) {
    return isSubtypeOf(subType.elementType_, superType.elementType_, cache);
  }
  return TypeResult::False;
}
#endif
