"use strict";

const { EventEmitter } = require("events");

const stores = new Map();

function defer(callback, context, ...args) {
  if (typeof callback !== "function") {
    return;
  }
  const run = () => callback.apply(context, args);
  if (typeof setImmediate === "function") {
    setImmediate(run);
  } else {
    Promise.resolve().then(run);
  }
}

function storeFor(filename) {
  const key = filename || ":memory:";
  let store = stores.get(key);
  if (!store) {
    store = new Map();
    stores.set(key, store);
  }
  return store;
}

function normalize(sql) {
  return String(sql || "").replace(/\s+/g, " ").trim().toUpperCase();
}

function parseCall(args) {
  const values = Array.from(args);
  let callback;
  if (typeof values[values.length - 1] === "function") {
    callback = values.pop();
  }
  let params = [];
  if (values.length === 1 && Array.isArray(values[0])) {
    params = values[0];
  } else if (values.length > 0) {
    params = values;
  }
  return { params, callback };
}

class Statement extends EventEmitter {
  constructor(database, sql) {
    super();
    this.database = database;
    this.sql = String(sql || "");
  }

  run(...args) {
    const { params, callback } = parseCall(args);
    const sql = normalize(this.sql);
    try {
      if (sql.startsWith("INSERT INTO ITEMTABLE")) {
        for (let index = 0; index < params.length; index += 2) {
          this.database.store.set(String(params[index]), params[index + 1]);
        }
      } else if (sql.startsWith("DELETE FROM ITEMTABLE")) {
        for (const key of params) {
          this.database.store.delete(String(key));
        }
      }
      defer(callback, this, null);
    } catch (error) {
      this.emit("error", error);
      defer(callback, this, error);
    }
    return this;
  }

  finalize(callback) {
    defer(callback, this, null);
  }
}

class Database extends EventEmitter {
  constructor(filename, mode, callback) {
    super();
    this.filename = filename || ":memory:";
    this.store = storeFor(this.filename);
    if (typeof mode === "function") {
      callback = mode;
    }
    defer(callback, this, null);
  }

  exec(sql, callback) {
    defer(callback, this, null);
    return this;
  }

  get(sql, ...args) {
    const { callback } = parseCall(args);
    const normalized = normalize(sql);
    let row;
    if (normalized.includes("PRAGMA INTEGRITY_CHECK")) {
      row = { integrity_check: "ok" };
    } else if (normalized.includes("PRAGMA QUICK_CHECK")) {
      row = { quick_check: "ok" };
    } else if (normalized.includes("PRAGMA USER_VERSION")) {
      row = { user_version: 1 };
    }
    defer(callback, this, null, row);
    return this;
  }

  all(sql, ...args) {
    const { callback } = parseCall(args);
    const normalized = normalize(sql);
    let rows = [];
    if (normalized.includes("FROM ITEMTABLE")) {
      rows = Array.from(this.store, ([key, value]) => ({ key, value }));
    }
    defer(callback, this, null, rows);
    return this;
  }

  run(sql, ...args) {
    const { callback } = parseCall(args);
    defer(callback, this, null);
    return this;
  }

  prepare(sql) {
    return new Statement(this, sql);
  }

  serialize(callback) {
    if (typeof callback === "function") {
      callback();
    }
    return this;
  }

  close(callback) {
    defer(callback, this, null);
  }
}

module.exports = {
  Database,
  OPEN_READONLY: 1,
  OPEN_READWRITE: 2,
  OPEN_CREATE: 4,
  verbose() {
    return module.exports;
  }
};
