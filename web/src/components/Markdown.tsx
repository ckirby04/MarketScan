// The little Markdown the assistant is allowed (system_prompt.md, Style):
// paragraphs, **bold**, `code`, bullet and numbered lists, and simple tables.
// Built as React elements, never as HTML, so nothing in a reply can become markup.
import type { ReactNode } from "react";

function inline(text: string): ReactNode[] {
  const out: ReactNode[] = [];
  const pattern = /\*\*([^*]+)\*\*|`([^`]+)`/g;
  let last = 0;
  let match;
  while ((match = pattern.exec(text)) !== null) {
    if (match.index > last) out.push(text.slice(last, match.index));
    out.push(
      match[1] !== undefined ? (
        <strong key={match.index}>{match[1]}</strong>
      ) : (
        <code key={match.index}>{match[2]}</code>
      ),
    );
    last = match.index + match[0].length;
  }
  if (last < text.length) out.push(text.slice(last));
  return out;
}

const cells = (row: string) =>
  row
    .trim()
    .replace(/^\||\|$/g, "")
    .split("|")
    .map((c) => c.trim());

export default function Markdown({ text }: { text: string }) {
  const blocks: ReactNode[] = [];
  const lines = text.replace(/\r\n/g, "\n").split("\n");
  let i = 0;
  while (i < lines.length) {
    const line = lines[i];
    if (!line.trim()) {
      i++;
      continue;
    }
    if (/^\s*[-*] /.test(line) || /^\s*\d+[.)] /.test(line)) {
      const ordered = /^\s*\d/.test(line);
      const items: string[] = [];
      while (i < lines.length && (/^\s*[-*] /.test(lines[i]) || /^\s*\d+[.)] /.test(lines[i]))) {
        items.push(lines[i].replace(/^\s*(?:[-*]|\d+[.)]) /, ""));
        i++;
      }
      const list = items.map((item, n) => <li key={n}>{inline(item)}</li>);
      blocks.push(ordered ? <ol key={i}>{list}</ol> : <ul key={i}>{list}</ul>);
      continue;
    }
    if (line.trim().startsWith("|")) {
      const rows: string[][] = [];
      while (i < lines.length && lines[i].trim().startsWith("|")) {
        if (!/^\s*\|?[\s:|-]+\|?\s*$/.test(lines[i])) rows.push(cells(lines[i]));
        i++;
      }
      if (rows.length === 0) continue;
      const [head, ...body] = rows;
      blocks.push(
        <table key={i} className="md-table">
          <thead>
            <tr>
              {head.map((c, n) => (
                <th key={n}>{inline(c)}</th>
              ))}
            </tr>
          </thead>
          <tbody>
            {body.map((row, r) => (
              <tr key={r}>
                {row.map((c, n) => (
                  <td key={n}>{inline(c)}</td>
                ))}
              </tr>
            ))}
          </tbody>
        </table>,
      );
      continue;
    }
    const paragraph: string[] = [];
    while (i < lines.length && lines[i].trim() && !/^\s*([-*] |\d+[.)] |\|)/.test(lines[i])) {
      paragraph.push(lines[i].replace(/^#+\s*/, ""));
      i++;
    }
    blocks.push(<p key={i}>{inline(paragraph.join(" "))}</p>);
  }
  return <div className="md">{blocks}</div>;
}
