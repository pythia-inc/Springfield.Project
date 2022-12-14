/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::units::{DeviceIntPoint, DeviceIntRect, DeviceIntSize};

//TODO: gather real-world statistics on the bin usage in order to assist the decision
// on where to place the size thresholds.

const NUM_BINS: usize = 3;
/// The minimum number of pixels on each side that we require for rects to be classified as
/// particular bin of freelists.
const MIN_RECT_AXIS_SIZES: [i32; NUM_BINS] = [1, 16, 32];

#[derive(Debug, Clone, Copy, PartialEq, PartialOrd)]
struct FreeListBin(u8);

#[derive(Debug, Clone, Copy)]
struct FreeListIndex(usize);

impl FreeListBin {
    fn for_size(size: &DeviceIntSize) -> Self {
        MIN_RECT_AXIS_SIZES
            .iter()
            .enumerate()
            .rev()
            .find(|(_, &min_size)| min_size <= size.width && min_size <= size.height)
            .map(|(id, _)| FreeListBin(id as u8))
            .unwrap_or_else(|| panic!("Unable to find a bin for {:?}!", size))
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct FreeRectSlice(pub u32);

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
struct FreeRect {
    slice: FreeRectSlice,
    rect: DeviceIntRect,
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
struct FreeRectSize {
    width: i16,
    height: i16,
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
struct Bin {
    // Store sizes with fewer bits per item and in a separate array to speed up
    // the search.
    sizes: Vec<FreeRectSize>,
    rects: Vec<FreeRect>,
}

/// A texture allocator using the guillotine algorithm.
///
/// See sections 2.2 and 2.2.5 in "A Thousand Ways to Pack the Bin - A Practical Approach to Two-
/// Dimensional Rectangle Bin Packing":
///
///    http://clb.demon.fi/files/RectangleBinPack.pdf
///
/// This approach was chosen because of its simplicity and good performance.
///
/// Note: the allocations are spread across multiple textures, and also are binned
/// orthogonally in order to speed up the search.
#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
pub struct GuillotineAllocator {
    bins: [Bin; NUM_BINS],
}

impl GuillotineAllocator {
    pub fn new(initial_size: Option<DeviceIntSize>) -> Self {
        let mut allocator = GuillotineAllocator {
            bins: [
                Bin { rects: Vec::new(), sizes: Vec::new() },
                Bin { rects: Vec::new(), sizes: Vec::new() },
                Bin { rects: Vec::new(), sizes: Vec::new() },
            ],
        };

        if let Some(initial_size) = initial_size {
            allocator.push(
                FreeRectSlice(0),
                initial_size.into(),
            );
        }

        allocator
    }

    fn push(&mut self, slice: FreeRectSlice, rect: DeviceIntRect) {
        let id = FreeListBin::for_size(&rect.size()).0 as usize;
        self.bins[id].rects.push(FreeRect {
            slice,
            rect,
        });
        self.bins[id].sizes.push(FreeRectSize {
            width: rect.width() as i16,
            height: rect.height() as i16,
        });
    }

    /// Find a suitable rect in the free list. We choose the first fit.
    fn find_index_of_best_rect(
        &self,
        requested_dimensions: &DeviceIntSize,
    ) -> Option<(FreeListBin, FreeListIndex)> {

        let start_bin = FreeListBin::for_size(&requested_dimensions);

        let w = requested_dimensions.width as i16;
        let h = requested_dimensions.height as i16;
        (start_bin.0 .. NUM_BINS as u8)
            .find_map(|id| {
                self.bins[id as usize].sizes
                    .iter()
                    .position(|candidate| w <= candidate.width && h <= candidate.height)
                    .map(|index| (FreeListBin(id), FreeListIndex(index)))
            })
    }

    // Split that results in the single largest area (Min Area Split Rule, MINAS).
    fn split_guillotine(&mut self, chosen: &FreeRect, requested_dimensions: &DeviceIntSize) {
        let candidate_free_rect_to_right = DeviceIntRect::from_origin_and_size(
            DeviceIntPoint::new(
                chosen.rect.min.x + requested_dimensions.width,
                chosen.rect.min.y,
            ),
            DeviceIntSize::new(
                chosen.rect.width() - requested_dimensions.width,
                requested_dimensions.height,
            ),
        );
        let candidate_free_rect_to_bottom = DeviceIntRect::from_origin_and_size(
            DeviceIntPoint::new(
                chosen.rect.min.x,
                chosen.rect.min.y + requested_dimensions.height,
            ),
            DeviceIntSize::new(
                requested_dimensions.width,
                chosen.rect.height() - requested_dimensions.height,
            ),
        );

        // Guillotine the rectangle.
        let new_free_rect_to_right;
        let new_free_rect_to_bottom;
        if candidate_free_rect_to_right.area() > candidate_free_rect_to_bottom.area() {
            new_free_rect_to_right = DeviceIntRect::from_origin_and_size(
                candidate_free_rect_to_right.min,
                DeviceIntSize::new(
                    candidate_free_rect_to_right.width(),
                    chosen.rect.height(),
                ),
            );
            new_free_rect_to_bottom = candidate_free_rect_to_bottom
        } else {
            new_free_rect_to_right = candidate_free_rect_to_right;
            new_free_rect_to_bottom = DeviceIntRect::from_origin_and_size(
                candidate_free_rect_to_bottom.min,
                DeviceIntSize::new(
                    chosen.rect.width(),
                    candidate_free_rect_to_bottom.height(),
                ),
            )
        }

        // Add the guillotined rects back to the free list.
        if !new_free_rect_to_right.is_empty() {
            self.push(chosen.slice, new_free_rect_to_right);
        }
        if !new_free_rect_to_bottom.is_empty() {
            self.push(chosen.slice, new_free_rect_to_bottom);
        }
    }

    pub fn allocate(
        &mut self, requested_dimensions: &DeviceIntSize
    ) -> Option<(FreeRectSlice, DeviceIntPoint)> {
        let mut requested_dimensions = *requested_dimensions;
        // Round up the size to a multiple of 8. This reduces the fragmentation
        // of the atlas.
        requested_dimensions.width = (requested_dimensions.width + 7) & !7;
        requested_dimensions.height = (requested_dimensions.height + 7) & !7;

        if requested_dimensions.width == 0 || requested_dimensions.height == 0 {
            return Some((FreeRectSlice(0), DeviceIntPoint::new(0, 0)));
        }

        let (bin, index) = self.find_index_of_best_rect(&requested_dimensions)?;

        // Remove the rect from the free list and decide how to guillotine it.
        let chosen = self.bins[bin.0 as usize].rects.swap_remove(index.0);
        self.bins[bin.0 as usize].sizes.swap_remove(index.0);
        self.split_guillotine(&chosen, &requested_dimensions);

        // Return the result.
        Some((chosen.slice, chosen.rect.min))
    }

    /// Add a new slice to the allocator, and immediately allocate a rect from it.
    pub fn extend(
        &mut self,
        slice: FreeRectSlice,
        total_size: DeviceIntSize,
        requested_dimensions: DeviceIntSize,
    ) {
        self.split_guillotine(
            &FreeRect { slice, rect: total_size.into() },
            &requested_dimensions
        );
    }
}

#[cfg(test)]
fn random_fill(count: usize, texture_size: i32) -> f32 {
    use rand::{thread_rng, Rng};

    let total_rect = DeviceIntRect::from_size(
        DeviceIntSize::new(texture_size, texture_size),
    );
    let mut rng = thread_rng();
    let mut allocator = GuillotineAllocator::new(None);

    // check for empty allocation
    assert_eq!(
        allocator.allocate(&DeviceIntSize::new(0, 12)),
        Some((FreeRectSlice(0), DeviceIntPoint::zero())),
    );

    let mut slices: Vec<Vec<DeviceIntRect>> = Vec::new();
    let mut requested_area = 0f32;
    // fill up the allocator
    for _ in 0 .. count {
        let size = DeviceIntSize::new(
            rng.gen_range(1, texture_size),
            rng.gen_range(1, texture_size),
        );
        requested_area += size.area() as f32;

        match allocator.allocate(&size) {
            Some((slice, origin)) => {
                let rect = DeviceIntRect::from_origin_and_size(origin, size);
                assert_eq!(None, slices[slice.0 as usize].iter().find(|r| r.intersects(&rect)));
                assert!(total_rect.contains_box(&rect));
                slices[slice.0 as usize].push(rect);
            }
            None => {
                allocator.extend(FreeRectSlice(slices.len() as u32), total_rect.size(), size);
                let rect = DeviceIntRect::from_size(size);
                slices.push(vec![rect]);
            }
        }
    }
    // validate the free rects
    for (i, bin) in allocator.bins.iter().enumerate() {
        for fr in &bin.rects {
            assert_eq!(FreeListBin(i as u8), FreeListBin::for_size(&fr.rect.size()));
            assert_eq!(None, slices[fr.slice.0 as usize].iter().find(|r| r.intersects(&fr.rect)));
            assert!(total_rect.contains_box(&fr.rect));
            slices[fr.slice.0 as usize].push(fr.rect);
        }
    }

    let allocated_area = slices.len() as f32 * (texture_size * texture_size) as f32;
    requested_area / allocated_area
}

#[test]
fn test_small() {
    random_fill(100, 100);
}

#[test]
fn test_large() {
    random_fill(1000, 10000);
}
