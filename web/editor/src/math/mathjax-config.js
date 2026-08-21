window.MathJax = {
  loader: {
    paths: {
      mathjax: "app://editor/mathjax",
      "mathjax-newcm": "app://editor/mathjax-font"
    },
    load: [
      "[tex]/ams", "[tex]/autoload", "[tex]/newcommand", "[tex]/configmacros",
      "[tex]/textmacros", "[tex]/require", "[tex]/mathtools", "[tex]/physics",
      "[tex]/mhchem", "[tex]/bussproofs", "[tex]/cancel", "[tex]/braket",
      "[tex]/color", "[tex]/cases", "[tex]/empheq", "[tex]/enclose",
      "[tex]/gensymb", "[tex]/upgreek", "[tex]/unicode", "[tex]/units",
      "a11y/assistive-mml"
    ]
  },
  tex: {
    inlineMath: [["$", "$"], ["\\(", "\\)"]],
    displayMath: [["$$", "$$"], ["\\[", "\\]"]],
    processEscapes: true,
    processEnvironments: true,
    tags: "ams",
    packages: {
      "[+]": [
        "ams", "autoload", "newcommand", "configmacros", "textmacros", "require",
        "mathtools", "physics", "mhchem", "bussproofs", "cancel", "braket",
        "color", "cases", "empheq", "enclose", "gensymb", "upgreek",
        "unicode", "units"
      ]
    }
  },
  chtml: {
    fontURL: "app://editor/mathjax-font/chtml/woff2"
  },
  options: {
    enableMenu: false,
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
