/**
 * Copyright (c) 2016 DeepCortex GmbH <legal@eventql.io>
 * Authors:
 *   - Laura Schlimmer <laura@eventql.io>
 *   - Paul Asmuth <paul@eventql.io>
 *
 * This program is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Affero General Public License ("the license") as
 * published by the Free Software Foundation, either version 3 of the License,
 * or any later version.
 *
 * In accordance with Section 7(e) of the license, the licensing of the Program
 * under the license does not imply a trademark license. Therefore any rights,
 * title and interest in our trademarks remain entirely with us.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the license for more details.
 *
 * You can be released from the requirements of the license by purchasing a
 * commercial license. Buying such a license is mandatory as soon as you develop
 * commercial activities involving this program without disclosing the source
 * code of your own applications
 */

zDomUtil = this.zDomUtil || {};

zDomUtil.replaceContent = function(elem, new_content) {
  elem.innerHTML = "";
  elem.appendChild(new_content);
}

zDomUtil.clearChildren = function(elem) {
  elem.innerHTML = "";
}

zDomUtil.onClick = function(elem, fn) {
  elem.addEventListener("click", function(e) {
    e.preventDefault();
    e.stopPropagation();
    fn.call(this, e);
    return false;
  });
};

zDomUtil.onEnter = function(elem, fn) {
  elem.addEventListener("keydown", function(e) {
    if (e.keyCode == 13) { //ENTER
      e.preventDefault();
      e.stopPropagation();
      fn.call(this, e);
      return false;
    }
  });
}

zDomUtil.handleLinks = function(elem, fn) {
  var click_fn = (function() {
    return function(e) {
      fn(this.getAttribute("href"));
      e.preventDefault();
      e.stopPropagation();
      return false;
    };
  })();

  var elems = elem.querySelectorAll("a");
  for (var i = 0; i < elems.length; ++i) {
    if (elems[i].hasAttribute("href")) {
      elems[i].addEventListener("click", click_fn);
    }
  }
};

zDomUtil.escapeHTML = function(str) {
  if (str == undefined || str == null || str.length == 0) {
    return "";
  }
  var div = document.createElement('div');
  div.appendChild(document.createTextNode(str));
  return div.innerHTML;
};

zDomUtil.nl2br = function(str) {
  return str.replace(/\n/g, "<br />");
};

zDomUtil.nl2p = function(str) {
  var lines = str.split("\n\n");

  return lines.map(function(s) {
    return "<p>" + s.replace(/\n/g, "<br />")  + "</p>";
  }).join("\n");
};

zDomUtil.textareaGetCursor = function(elem) {
  if ("selectionStart" in elem && document.activeElement == elem) {
    return elem.selectionStart;
  }

  if (elem.createTextRange) {
    var s = document.section.createRange();
    s.moveStart ("character", -elem.value.length);
    return s.text.split("\n").join("").length;
  }

  return -1;
}

zDomUtil.textareaSetCursor = function(elem, pos) {
  if ("selectionStart" in elem) {
    setTimeout(function() {
      elem.selectionStart = pos;
      elem.selectionEnd = pos;
    }, 1);
    return;
  }

  if (elem.createTextRange) {
    var rng = elem.createTextRange();
    rng.moveStart("character", pos);
    rng.collapse();
    rng.moveEnd("character", 0);
    rng.select();
    return;
  }
}
