"use client";

// The form by hand: a Strategy Form filled in field by field, and Run. The
// assistant (the home page) fills the same form from a conversation; this page
// is for checking or editing one directly.
//
// The service validates it against the same contract the engine does and either
// starts a job (and the page moves to /runs/<id>) or answers with every problem
// by form path, shown under the field it names. `?from=<job>` starts from that
// job's form instead of the starter.
import { useRouter, useSearchParams } from "next/navigation";
import { Suspense, useEffect, useState } from "react";

import starterJson from "@/contracts/starter_form.json";
import schemaJson from "@/contracts/strategy_form.schema.json";
import FormEditor, { type FieldErrors } from "@/components/FormEditor";
import Rail from "@/components/Rail";
import type { JsonObject } from "@/lib/json";

// Documents, read as documents: the editor walks them, it does not type them.
const schema = schemaJson as unknown as JsonObject;
const starter = starterJson as unknown as JsonObject;

function FormPage() {
  const router = useRouter();
  const from = useSearchParams().get("from");
  const [form, setForm] = useState<JsonObject>(starter);
  const [errors, setErrors] = useState<FieldErrors>({});
  const [problem, setProblem] = useState("");
  const [sending, setSending] = useState(false);

  useEffect(() => {
    if (!from) return;
    fetch(`/api/jobs/${from}/form`)
      .then((r) => (r.ok ? r.json() : Promise.reject(new Error(`could not load job ${from}'s form`))))
      .then((loaded: JsonObject) => setForm(loaded))
      .catch((e: Error) => setProblem(e.message));
  }, [from]);

  async function run() {
    setSending(true);
    setProblem("");
    setErrors({});
    try {
      const response = await fetch("/api/jobs", { method: "POST", body: JSON.stringify(form) });
      const body = await response.json();
      if (response.status === 202) {
        router.push(`/runs/${body.job_id}`);
        return;
      }
      if (response.status === 422) {
        const byPath: FieldErrors = {};
        for (const e of body.errors as { path: string; message: string }[]) {
          (byPath[e.path] ??= []).push(e.message);
        }
        setErrors(byPath);
        setProblem(`${body.errors.length} problem(s) with the form; each is shown under its field.`);
        return;
      }
      setProblem(body.error ?? body.detail ?? `the service answered ${response.status}`);
    } catch (e) {
      setProblem((e as Error).message);
    } finally {
      setSending(false);
    }
  }

  return (
    <div className="shell">
      <Rail />
      <main className="main">
        <FormEditor schema={schema} form={form} errors={errors} onChange={setForm} />
        <div className="heading">RUN</div>
        {problem && <p className="error">! {problem}</p>}
        {Object.entries(errors)
          .filter(([path]) => path === "/" || path === "")
          .map(([path, messages]) => messages.map((m) => <p key={path + m} className="error">! {m}</p>))}
        <button className="primary" onClick={run} disabled={sending}>
          {sending ? "Starting..." : "Run"}
        </button>
      </main>
    </div>
  );
}

export default function Page() {
  return (
    <Suspense>
      <FormPage />
    </Suspense>
  );
}
