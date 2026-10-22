// Copies the contracts the front end reads from ../contracts into src/contracts.
//
// The schemas live once, in the repository's contracts/ directory, and the
// engine, the research layer, the service and this app all read that copy. This
// runs before `dev` and `build` so the app never carries a fork of them; the
// copied directory is gitignored.
import { copyFileSync, mkdirSync } from "node:fs";
import { dirname, join } from "node:path";
import { fileURLToPath } from "node:url";

const web = join(dirname(fileURLToPath(import.meta.url)), "..");
const contracts = join(web, "..", "contracts", "research");
const out = join(web, "src", "contracts");

mkdirSync(out, { recursive: true });
for (const [from, to] of [
  ["strategy_form.schema.json", "strategy_form.schema.json"],
  ["results.schema.json", "results.schema.json"],
  [join("examples", "starter_form.json"), "starter_form.json"],
]) {
  copyFileSync(join(contracts, from), join(out, to));
}
console.log(`contracts synced to ${out}`);
