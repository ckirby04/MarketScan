// A JSON value, typed. Forms and Results are documents, not classes.
export type Json = null | boolean | number | string | Json[] | { [key: string]: Json };
export type JsonObject = { [key: string]: Json };

export function isObject(value: Json | undefined): value is JsonObject {
  return typeof value === "object" && value !== null && !Array.isArray(value);
}

export function clone<T extends Json>(value: T): T {
  return JSON.parse(JSON.stringify(value)) as T;
}
