// |jit-test| allow-oom; allow-unhandlable-oom
// Bug 1234402
// Unhandlable OOM in AlternativeGeneration::AlternativeGeneration.

if (typeof oomAfterAllocations == "function" && helperThreadCount() > 0) {
  offThreadCompileToStencil(`
[null, "", ""].forEach(function(locales) {
try {
Intl.NumberFormat(locales)
} catch (e) {}
oomAfterAllocations(100);
})
`);
  var stencil = finishOffThreadStencil();
  evalStencil(stencil);
}
