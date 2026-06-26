"use strict";

function asArray(value) {
  if (typeof value === "undefined" || value === null || value === false) {
    return [];
  }
  return Array.isArray(value) ? value : [value];
}

function optionSet(value) {
  const result = Object.create(null);
  for (const item of asArray(value)) {
    result[String(item)] = true;
  }
  return result;
}

function buildAliases(aliasOption) {
  const aliases = Object.create(null);

  function add(left, right) {
    left = String(left);
    right = String(right);
    aliases[left] = aliases[left] || [];
    aliases[right] = aliases[right] || [];
    if (!aliases[left].includes(right)) {
      aliases[left].push(right);
    }
    if (!aliases[right].includes(left)) {
      aliases[right].push(left);
    }
  }

  for (const key of Object.keys(aliasOption || {})) {
    for (const alias of asArray(aliasOption[key])) {
      add(key, alias);
    }
  }

  return aliases;
}

function expandedKeys(key, aliases) {
  const result = [key];
  for (const alias of aliases[key] || []) {
    if (!result.includes(alias)) {
      result.push(alias);
    }
  }
  return result;
}

function hasOption(key, set, aliases) {
  if (set[key]) {
    return true;
  }
  return (aliases[key] || []).some(alias => set[alias]);
}

function coerceValue(value, key, strings, aliases) {
  if (hasOption(key, strings, aliases)) {
    return String(value);
  }
  if (value === "true") {
    return true;
  }
  if (value === "false") {
    return false;
  }
  if (typeof value === "string" && value.trim() !== "" && /^-?(?:0|[1-9]\d*)(?:\.\d+)?$/.test(value)) {
    return Number(value);
  }
  return value;
}

function setValue(out, aliases, key, value) {
  for (const name of expandedKeys(key, aliases)) {
    if (typeof out[name] === "undefined") {
      out[name] = value;
    } else if (Array.isArray(out[name])) {
      out[name].push(value);
    } else {
      out[name] = [out[name], value];
    }
  }
}

function setDefault(out, aliases, key, value) {
  for (const name of expandedKeys(key, aliases)) {
    if (typeof out[name] === "undefined") {
      out[name] = value;
    }
  }
}

module.exports = function minimist(args, opts) {
  opts = opts || {};
  args = Array.prototype.slice.call(args || []);

  const out = { _: [] };
  const aliases = buildAliases(opts.alias);
  const strings = optionSet(opts.string);
  const booleans = opts.boolean === true ? Object.assign(Object.create(null), { __all: true }) : optionSet(opts.boolean);

  function isString(key) {
    return hasOption(key, strings, aliases);
  }

  function isBoolean(key) {
    return booleans.__all || hasOption(key, booleans, aliases);
  }

  function readValue(key, index) {
    if (isBoolean(key)) {
      return { value: true, index };
    }
    const next = args[index + 1];
    if (typeof next !== "undefined" && next !== "--" && next[0] !== "-") {
      return { value: coerceValue(next, key, strings, aliases), index: index + 1 };
    }
    return { value: isString(key) ? "" : true, index };
  }

  for (let i = 0; i < args.length; i++) {
    const arg = args[i];

    if (arg === "--") {
      out._.push(...args.slice(i + 1));
      break;
    }

    if (arg.length > 2 && arg.slice(0, 2) === "--") {
      const body = arg.slice(2);
      const eq = body.indexOf("=");
      if (body.slice(0, 3) === "no-" && eq === -1) {
        setValue(out, aliases, body.slice(3), false);
      } else if (eq !== -1) {
        const key = body.slice(0, eq);
        setValue(out, aliases, key, coerceValue(body.slice(eq + 1), key, strings, aliases));
      } else {
        const parsed = readValue(body, i);
        setValue(out, aliases, body, parsed.value);
        i = parsed.index;
      }
      continue;
    }

    if (arg.length > 1 && arg[0] === "-") {
      const body = arg.slice(1);
      const eq = body.indexOf("=");
      if (eq !== -1) {
        const key = body.slice(0, eq);
        setValue(out, aliases, key, coerceValue(body.slice(eq + 1), key, strings, aliases));
        continue;
      }

      let consumed = false;
      for (let j = 0; j < body.length; j++) {
        const key = body[j];
        const rest = body.slice(j + 1);
        if (rest && isString(key)) {
          setValue(out, aliases, key, coerceValue(rest, key, strings, aliases));
          consumed = true;
          break;
        }
        if (j === body.length - 1) {
          const parsed = readValue(key, i);
          setValue(out, aliases, key, parsed.value);
          i = parsed.index;
        } else {
          setValue(out, aliases, key, true);
        }
      }
      if (consumed) {
        continue;
      }
      continue;
    }

    out._.push(coerceValue(arg, "_", strings, aliases));
  }

  for (const key of Object.keys(opts.default || {})) {
    setDefault(out, aliases, key, opts.default[key]);
  }

  if (!booleans.__all) {
    for (const key of Object.keys(booleans)) {
      setDefault(out, aliases, key, false);
    }
  }

  return out;
};
