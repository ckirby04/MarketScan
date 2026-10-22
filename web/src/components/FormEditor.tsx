"use client";

// A Strategy Form, edited, drawn from strategy_form.schema.json.
//
// One widget per kind of schema node -- a choice for oneOf and enum, nested
// objects, add and remove for optional properties and array items -- and none
// written for a particular field. Provenance shows beside a field as a word,
// never colour alone; the service's validation errors show under the field they
// name.
import { clone, isObject, type Json, type JsonObject } from "@/lib/json";
import {
  branchLabel,
  branchOf,
  defaultFor,
  labelFor,
  pointerToken,
  propertyOrder,
  resolve,
} from "@/lib/schemaForm";

export type FieldErrors = Record<string, string[]>;

interface Context {
  root: JsonObject;
  provenance: JsonObject;
  errors: FieldErrors;
}

function Badge({ ctx, path }: { ctx: Context; path: string }) {
  const entry = ctx.provenance[path];
  if (!isObject(entry) || typeof entry.source !== "string") return null;
  const tip = [entry.quote ? `"${String(entry.quote)}"` : "", entry.note ? String(entry.note) : ""]
    .filter(Boolean)
    .join("\n");
  return (
    <span className={`badge ${entry.source === "missing" ? "serious" : ""}`} title={tip}>
      {entry.source === "missing" ? "MISSING" : entry.source}
    </span>
  );
}

function Errors({ ctx, path }: { ctx: Context; path: string }) {
  const messages = ctx.errors[path];
  if (!messages) return null;
  return (
    <>
      {messages.map((m) => (
        <div key={m} className="error">
          ! {m}
        </div>
      ))}
    </>
  );
}

function Scalar({
  node,
  value,
  onChange,
}: {
  node: JsonObject;
  value: Json;
  onChange: (v: Json) => void;
}) {
  if ("const" in node) return <span className="faint mono">{JSON.stringify(node.const)}</span>;
  if (Array.isArray(node.enum)) {
    return (
      <select value={JSON.stringify(value)} onChange={(e) => onChange(JSON.parse(e.target.value))}>
        {node.enum.map((option) => (
          <option key={JSON.stringify(option)} value={JSON.stringify(option)}>
            {typeof option === "string" ? option : JSON.stringify(option)}
          </option>
        ))}
      </select>
    );
  }
  switch (node.type) {
    case "boolean":
      return <input type="checkbox" checked={value === true} onChange={(e) => onChange(e.target.checked)} />;
    case "integer":
    case "number":
      return (
        <input
          type="number"
          step={node.type === "integer" ? 1 : "any"}
          value={typeof value === "number" ? value : ""}
          onChange={(e) => onChange(e.target.value === "" ? null : Number(e.target.value))}
          style={{ width: 120 }}
        />
      );
    default:
      return (
        <input
          type="text"
          value={typeof value === "string" ? value : ""}
          placeholder={node.format === "date" ? "YYYY-MM-DD" : undefined}
          onChange={(e) => onChange(e.target.value)}
          style={{ width: 220 }}
        />
      );
  }
}

function Value({
  ctx,
  raw,
  value,
  path,
  label,
  onChange,
}: {
  ctx: Context;
  raw: Json;
  value: Json;
  path: string;
  label: string;
  onChange: (v: Json) => void;
}) {
  const node = resolve(ctx.root, raw);
  if (!isObject(node)) return null;

  if (Array.isArray(node.oneOf)) {
    const branches = node.oneOf;
    const current = branchOf(ctx.root, branches, value);
    const branch = resolve(ctx.root, branches[current]);
    return (
      <>
        <div className="field">
          <label>{label}</label>
          <span>
            <select
              value={current}
              onChange={(e) => onChange(defaultFor(ctx.root, branches[Number(e.target.value)]))}
            >
              {branches.map((b, i) => (
                <option key={i} value={i}>
                  {branchLabel(ctx.root, b)}
                </option>
              ))}
            </select>{" "}
            {!isObject(value) && isObject(branch) && branch.type !== "null" && (
              <Scalar node={branch} value={value} onChange={onChange} />
            )}
            <Badge ctx={ctx} path={path} />
          </span>
        </div>
        {isObject(value) && isObject(branch) && (
          <div className="nested">
            <ObjectFields ctx={ctx} node={branch} value={value} path={path} onChange={onChange} />
          </div>
        )}
        <Errors ctx={ctx} path={path} />
      </>
    );
  }

  if (node.type === "object") {
    return (
      <>
        <div className="field">
          <label>{label}</label>
          <span>
            <Badge ctx={ctx} path={path} />
          </span>
        </div>
        <div className="nested">
          <ObjectFields ctx={ctx} node={node} value={isObject(value) ? value : {}} path={path} onChange={onChange} />
        </div>
        <Errors ctx={ctx} path={path} />
      </>
    );
  }

  if (node.type === "array") {
    const items = Array.isArray(value) ? value : [];
    const min = typeof node.minItems === "number" ? node.minItems : 0;
    const max = typeof node.maxItems === "number" ? node.maxItems : 64;
    return (
      <>
        <div className="field">
          <label>{label}</label>
          <span>
            {items.length < max && (
              <button
                className="small"
                type="button"
                onClick={() => onChange([...items, defaultFor(ctx.root, node.items ?? {})])}
              >
                + add
              </button>
            )}
            <Badge ctx={ctx} path={path} />
          </span>
        </div>
        <div className="nested">
          {items.map((item, i) => (
            <div key={i}>
              <Value
                ctx={ctx}
                raw={node.items ?? {}}
                value={item}
                path={`${path}/${i}`}
                label={`#${i + 1}`}
                onChange={(v) => onChange(items.map((x, j) => (j === i ? v : x)))}
              />
              {items.length > min && (
                <button
                  className="small"
                  type="button"
                  onClick={() => onChange(items.filter((_, j) => j !== i))}
                >
                  remove #{i + 1}
                </button>
              )}
            </div>
          ))}
        </div>
        <Errors ctx={ctx} path={path} />
      </>
    );
  }

  return (
    <>
      <div className="field">
        <label>{label}</label>
        <span>
          <Scalar node={node} value={value} onChange={onChange} />
          <Badge ctx={ctx} path={path} />
        </span>
      </div>
      <Errors ctx={ctx} path={path} />
    </>
  );
}

function ObjectFields({
  ctx,
  node,
  value,
  path,
  onChange,
}: {
  ctx: Context;
  node: JsonObject;
  value: JsonObject;
  path: string;
  onChange: (v: Json) => void;
}) {
  const props = isObject(node.properties) ? node.properties : {};
  const required = Array.isArray(node.required) ? node.required.map(String) : [];
  return (
    <>
      {propertyOrder(node).map((key) => {
        const prop = resolve(ctx.root, props[key]);
        // A constant is shown by whatever chose it, and is not editable alone.
        if (isObject(prop) && "const" in prop) return null;
        const child = `${path}/${pointerToken(key)}`;
        const set = (v: Json) => onChange({ ...value, [key]: v });
        if (key in value) {
          return (
            <div key={key}>
              <Value ctx={ctx} raw={props[key]} value={value[key]} path={child} label={labelFor(key)} onChange={set} />
              {!required.includes(key) && (
                <button
                  className="small"
                  type="button"
                  onClick={() => {
                    const next = clone(value);
                    delete next[key];
                    onChange(next);
                  }}
                >
                  remove {labelFor(key)}
                </button>
              )}
            </div>
          );
        }
        return required.includes(key) ? null : (
          <div key={key}>
            <button className="small" type="button" onClick={() => set(defaultFor(ctx.root, props[key]))}>
              + {labelFor(key)}
            </button>
          </div>
        );
      })}
    </>
  );
}

const SECTIONS: [string, string[]][] = [
  ["QUESTION", ["prompt"]],
  ["ASSET", ["universe"]],
  ["DATES", ["date_range"]],
  ["STRATEGY", ["template", "strategy"]],
  ["JUDGED BY", ["objective", "benchmarks"]],
  ["CAPITAL, COSTS, CASH, EXECUTION, ROBUSTNESS", ["capital", "costs", "cash", "execution", "robustness"]],
];

export default function FormEditor({
  schema,
  form,
  errors,
  onChange,
}: {
  schema: JsonObject;
  form: JsonObject;
  errors: FieldErrors;
  onChange: (form: JsonObject) => void;
}) {
  const ctx: Context = {
    root: schema,
    provenance: isObject(form.provenance) ? form.provenance : {},
    errors,
  };
  const props = isObject(schema.properties) ? schema.properties : {};
  return (
    <>
      {SECTIONS.map(([title, keys]) => (
        <section key={title}>
          <div className="heading">{title}</div>
          {keys
            .filter((key) => key in form)
            .map((key) => {
              const node = resolve(schema, props[key]);
              const path = `/${key}`;
              const set = (v: Json) => onChange({ ...form, [key]: v });
              return isObject(node) && node.type === "object" && isObject(form[key]) ? (
                <div key={key}>
                  <div className="faint">{labelFor(key)}</div>
                  <ObjectFields ctx={ctx} node={node} value={form[key] as JsonObject} path={path} onChange={set} />
                  <Errors ctx={ctx} path={path} />
                </div>
              ) : (
                <Value key={key} ctx={ctx} raw={props[key]} value={form[key]} path={path} label={labelFor(key)} onChange={set} />
              );
            })}
        </section>
      ))}
    </>
  );
}
