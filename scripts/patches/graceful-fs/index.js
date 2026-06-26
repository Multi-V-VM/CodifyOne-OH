"use strict";

const fs = require("fs");

function gracefulify(target) {
  return target || fs;
}

module.exports = fs;
module.exports.gracefulify = gracefulify;
