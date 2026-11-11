// Reveal sections as they scroll into view.
(function () {
    var items = document.querySelectorAll('.reveal');
    if (!('IntersectionObserver' in window)) {
        items.forEach(function (el) { el.classList.add('in'); });
        return;
    }
    var io = new IntersectionObserver(function (entries) {
        entries.forEach(function (entry) {
            if (entry.isIntersecting) {
                entry.target.classList.add('in');
                io.unobserve(entry.target);
            }
        });
    }, { threshold: 0.12 });
    items.forEach(function (el) { io.observe(el); });
})();

// Copy the quick-start commands to the clipboard.
(function () {
    var button = document.querySelector('.code .copy');
    if (!button || !navigator.clipboard) return;
    button.addEventListener('click', function () {
        var text = button.parentElement.querySelector('pre').innerText;
        navigator.clipboard.writeText(text).then(function () {
            button.textContent = 'Copied';
            setTimeout(function () { button.textContent = 'Copy'; }, 1600);
        });
    });
})();