// What the form editor needs to know about a JSON Schema.
//
// The same decisions the desktop app's apps/marketscan/schema_form.cpp makes,
// for the same schema: follow $ref, build a default, tell which oneOf branch a
// value is, name a branch, order an object's properties. The form is drawn from
// the schema, one widget per kind of node, and no widget is written for a
// particular field -- adding a template must not need front-end code.
import { isObject, type Json, type JsonObject } from "./json";

export function resolve(root: JsonObject, node: Json): Json {
  let current = node;
  for (let depth = 0; depth < 32 && isObject(current) && typeof current.$ref === "string"; depth++) {
    const tokens = current.$ref.slice(2).split("/").map((t) => t.replace(/~1/g, "/").replace(/~0/g, "~"));
    let target: Json = root;
    for (const token of tokens) {
      target = isObject(target) ? target[token] : null;
    }
    current = target;
  }
  return current;
}

function typeOf(node: JsonObject): string {
  return typeof node.type === "string" ? node.type : "";
}

function typeMatches(node: JsonObject, value: Json): boolean {
  if (node.type === undefined) return true;
  const one = (type: string): boolean => {
    switch (type) {
      case "number":
        return typeof value === "number";
      case "integer":
        return typeof value === "number" && Number.isInteger(value);
      case "string":
        return typeof value === "string";
      case "boolean":
        return typeof value === "boolean";
      case "object":
        return isObject(value);
      case "array":
        return Array.isArray(value);
      default:
        return value === null;
    }
  };
  return Array.isArray(node.type) ? node.type.some((t) => one(String(t))) : one(String(node.type));
}

function equal(a: Json | undefined, b: Json | undefined): boolean {
  return JSON.stringify(a) === JSON.stringify(b);
}

function matches(root: JsonObject, raw: Json, value: Json): boolean {
  const node = resolve(root, raw);
  if (typeof node === "boolean") return node;
  if (!isObject(node)) return false;
  if (!typeMatches(node, value)) return false;
  if ("const" in node && !equal(node.const, value)) return false;
  if (Array.isArray(node.oneOf) && !node.oneOf.some((b) => matches(root, b, value))) return false;
  if (isObject(value)) {
    const props = isObject(node.properties) ? node.properties : {};
    for (const [key, prop] of Object.entries(props)) {
      const p = resolve(root, prop);
      if (isObject(p) && "const" in p && !equal(value[key], p.const)) return false;
    }
    if (Array.isArray(node.required) && !node.required.every((k) => String(k) in value)) return false;
    if (node.additionalProperties === false && !Object.keys(value).every((k) => k in props)) return false;
  }
  return true;
}

export function defaultFor(root: JsonObject, raw: Json): Json {
  const node = resolve(root, raw);
  if (!isObject(node)) return null;
  if ("const" in node) return node.const;
  if ("default" in node) return node.default;
  const enumValues = Array.isArray(node.enum) ? node.enum : null;
  const xDefault = node["x-default"];
  if (
    xDefault !== undefined &&
    node.type !== undefined &&
    typeMatches(node, xDefault) &&
    (!enumValues || enumValues.some((e) => equal(e, xDefault)))
  ) {
    return xDefault;
  }
  if (enumValues) return enumValues[0];
  if (Array.isArray(node.oneOf)) return defaultFor(root, node.oneOf[0]);
  switch (typeOf(node)) {
    case "object": {
      const out: JsonObject = {};
      const props = isObject(node.properties) ? node.properties : {};
      for (const key of Array.isArray(node.required) ? node.required : []) {
        out[String(key)] = defaultFor(root, props[String(key)]);
      }
      return out;
    }
    case "array": {
      const count = typeof node.minItems === "number" ? node.minItems : 0;
      return Array.from({ length: count }, () => defaultFor(root, node.items ?? {}));
    }
    case "number":
    case "integer": {
      if (typeof node.minimum === "number") return node.minimum;
      if (typeof node.exclusiveMinimum === "number") return node.exclusiveMinimum + 1;
      return 0;
    }
    case "boolean":
      return false;
    case "string":
      return node.format === "date" ? "2000-01-03" : "";
    default:
      return null;
  }
}

export function branchOf(root: JsonObject, oneOf: Json[], value: Json): number {
  const index = oneOf.findIndex((branch) => matches(root, branch, value));
  return index < 0 ? 0 : index;
}

export function branchLabel(root: JsonObject, raw: Json): string {
  const node = resolve(root, raw);
  if (!isObject(node)) return "option";
  if ("const" in node) return typeof node.const === "string" ? node.const : JSON.stringify(node.const);
  if (isObject(node.properties)) {
    for (const prop of Object.values(node.properties)) {
      const p = resolve(root, prop);
      if (isObject(p) && typeof p.const === "string") return p.const;
    }
  }
  if (Array.isArray(node.required) && node.required.length > 0) return String(node.required[0]);
  const type = typeOf(node);
  if (type === "null") return "none";
  if (type === "number" || type === "integer") return "value";
  if (type === "string") return typeof node.format === "string" ? node.format : "text";
  return type || "option";
}

/** Its `required` list as written, then the rest by name: the author's order survives there. */
export function propertyOrder(node: JsonObject): string[] {
  const props = isObject(node.properties) ? node.properties : {};
  const out = (Array.isArray(node.required) ? node.required : [])
    .map(String)
    .filter((key) => key in props);
  for (const key of Object.keys(props).sort()) {
    if (!out.includes(key)) out.push(key);
  }
  return out;
}

export function labelFor(key: string): string {
  const words = key.replace(/_/g, " ");
  return words.endsWith(" pct") ? `${words.slice(0, -4)} %` : words;
}

export function pointerToken(key: string): string {
  return key.replace(/~/g, "~0").replace(/\//g, "~1");
}
