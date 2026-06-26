"use strict";

const keyboardLayoutInfo = {
  id: "ohos.us",
  lang: "en-US",
  localizedName: "US",
  layout: "US"
};

exports.getCurrentKeyboardLayout = function() {
  return keyboardLayoutInfo;
};

exports.getKeyMap = function() {
  return [];
};

exports.onDidChangeKeyboardLayout = function() {
  return undefined;
};

exports.isISOKeyboard = function() {
  return false;
};
