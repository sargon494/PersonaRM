// Reveal sections as they scroll into view; items revealed together stagger by 60 ms (capped).
(function () {
    document.documentElement.classList.add('js');
    var items = document.querySelectorAll('.reveal');
    if (!('IntersectionObserver' in window)) {
        items.forEach(function (el) { el.classList.add('in'); });
        return;
    }
    var io = new IntersectionObserver(function (entries) {
        var n = 0;
        entries.forEach(function (entry) {
            if (entry.isIntersecting) {
                entry.target.style.setProperty('--d', Math.min(n++, 6) * 60 + 'ms');
                entry.target.classList.add('in');
                io.unobserve(entry.target);
            }
        });
    }, { threshold: 0.12 });
    items.forEach(function (el) { io.observe(el); });
})();

// Copy the developer commands: Clipboard API, then execCommand, then a visible failure state.
(function () {
    var button = document.querySelector('.code .copy');
    if (!button) return;
    var label = button.textContent;
    function done(ok) {
        button.textContent = ok ? (button.dataset.copied || 'Copied') : (button.dataset.failed || 'Copy failed');
        button.classList.toggle('failed', !ok);
        setTimeout(function () { button.textContent = label; button.classList.remove('failed'); }, ok ? 1600 : 3500);
    }
    function legacy(text) {
        var ta = document.createElement('textarea');
        ta.value = text;
        ta.setAttribute('readonly', '');
        ta.style.cssText = 'position:fixed;top:0;left:-9999px';
        document.body.appendChild(ta);
        ta.select();
        var ok = false;
        try { ok = document.execCommand('copy'); } catch (e) { }
        document.body.removeChild(ta);
        done(ok);
    }
    button.setAttribute('aria-live', 'polite');
    button.addEventListener('click', function () {
        var text = button.parentElement.querySelector('pre').innerText;
        if (navigator.clipboard && navigator.clipboard.writeText) {
            navigator.clipboard.writeText(text).then(function () { done(true); }, function () { legacy(text); });
        } else {
            legacy(text);
        }
    });
})();
