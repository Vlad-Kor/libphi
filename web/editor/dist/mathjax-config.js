window.MathJax = {
  loader: {
    paths: {
      mathjax: "app://editor/mathjax",
      "mathjax-newcm": "app://editor/mathjax-font",
      "mathjax-dsfont-extension": "app://editor/mathjax-dsfont-font-extension",
      "mathjax-mhchem-extension": "app://editor/mathjax-mhchem-font-extension"
    },
    load: [
      "[tex]/ams", "[tex]/autoload", "[tex]/newcommand", "[tex]/configmacros",
      "[tex]/textmacros", "[tex]/mathtools", "[tex]/physics",
      "[tex]/dsfont", "[tex]/mhchem", "[tex]/bussproofs", "[tex]/cancel", "[tex]/braket",
      "[tex]/color", "[tex]/cases", "[tex]/empheq", "[tex]/enclose",
      "[tex]/gensymb", "[tex]/upgreek", "[tex]/unicode", "[tex]/units",
    ],
    failed: function (error) {
      window.__phiMathJaxError = error && error.message
        ? error.message
        : String(error || "MathJax component failed to load");
      console.error("MathJax:", window.__phiMathJaxError);
    }
  },
  tex: {
    inlineMath: [["$", "$"], ["\\(", "\\)"]],
    displayMath: [["$$", "$$"], ["\\[", "\\]"]],
    processEscapes: true,
    processEnvironments: true,
    tags: "none",
    require: {
      prefix: "tex"
    },
    packages: {
      "[+]": [
        "ams", "autoload", "newcommand", "configmacros", "textmacros",
        "mathtools", "physics", "dsfont", "mhchem", "bussproofs", "cancel", "braket",
        "color", "cases", "empheq", "enclose", "gensymb", "upgreek",
        "unicode", "units"
      ]
    }
  },
  svg: {
    fontCache: "local",
    linebreaks: {
      inline: false
    }
  },
  options: {
    enableMenu: false,
    enableAssistiveMml: false,
    safeOptions: {
      allow: {
        URLs: "safe",
        classes: "safe",
        cssIDs: "safe",
        styles: "safe"
      }
    }
  },
  startup: { typeset: false }
};
