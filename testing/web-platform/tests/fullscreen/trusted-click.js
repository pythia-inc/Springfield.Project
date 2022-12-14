// Invokes callback from a trusted click event.
//
// TODO(mustaq): This method needs a cleanup.  This is based on an old version
// of the HTML spec.  The new spec doesn't require a callback because the user
// activation is visible across the Window object:
// https://html.spec.whatwg.org/multipage/interaction.html#tracking-user-activation.
function trusted_click(test, callback, container)
{
    var document = container.ownerDocument;
    var button = document.createElement("button");
    button.textContent = "click to continue test";
    button.style.display = "block";
    button.style.fontSize = "20px";
    button.style.padding = "10px";
    button.onclick = test.step_func(function()
    {
        callback();
        container.removeChild(button);
    });
    container.appendChild(button);
}

// Invokes element.requestFullscreen() from a trusted click.
function trusted_request(test, element, container)
{
    trusted_click(test, () => {
        var promise = element.requestFullscreen();
        if (promise) {
            // Keep the promise resolution silent. Otherwise unhandledrejection
            // may fire for the failure test cases.
            promise.then(() => {}, () => {});
        }
    }, container || element.parentNode);
}

// Invokes element.requestFullscreen() from a trusted click.
function trusted_request_with_promise(test, element, container, resolve, reject)
{
    trusted_click(test, () => {
        element.requestFullscreen().then(resolve, reject);
    }, container || element.parentNode);
}
