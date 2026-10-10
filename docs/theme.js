/* Optional theme switch for the Tessel documentation pages.
   Every page is readable with this file blocked or JavaScript turned off:
   without it the dark palette is used, or the light one when the system
   asks for a light appearance. */
(function () {
    "use strict";

    var KEY = "tessel-docs-theme";

    function stored() {
        try {
            return window.localStorage.getItem(KEY);
        } catch (error) {
            return null;
        }
    }

    function remember(value) {
        try {
            window.localStorage.setItem(KEY, value);
        } catch (error) {
            /* Private windows and blocked storage are fine, the choice just
               does not survive the next page load. */
        }
    }

    function systemPrefersLight() {
        return typeof window.matchMedia === "function"
            && window.matchMedia("(prefers-color-scheme: light)").matches;
    }

    function current() {
        var explicit = document.documentElement.getAttribute("data-theme");
        if (explicit === "light" || explicit === "dark") {
            return explicit;
        }
        return systemPrefersLight() ? "light" : "dark";
    }

    /* Runs while the document is still being parsed, so the stored choice is
       in place before anything is painted. */
    var saved = stored();
    if (saved === "light" || saved === "dark") {
        document.documentElement.setAttribute("data-theme", saved);
    }

    function label(theme) {
        return theme === "light" ? "Dark theme" : "Light theme";
    }

    function addButton() {
        var list = document.querySelector(".site-nav ul");
        if (!list) {
            return;
        }
        var item = document.createElement("li");
        var button = document.createElement("button");
        button.type = "button";
        button.className = "theme-toggle";
        button.textContent = label(current());
        button.setAttribute("aria-live", "polite");
        button.addEventListener("click", function () {
            var next = current() === "light" ? "dark" : "light";
            document.documentElement.setAttribute("data-theme", next);
            remember(next);
            button.textContent = label(next);
        });
        item.appendChild(button);
        list.appendChild(item);
    }

    if (document.readyState === "loading") {
        document.addEventListener("DOMContentLoaded", addButton);
    } else {
        addButton();
    }
}());
