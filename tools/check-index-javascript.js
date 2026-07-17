const fs = require("node:fs");
const path = require("node:path");

const root = path.resolve(__dirname, "..");
const html = fs.readFileSync(path.join(root, "index.html"), "utf8");
const scriptPattern = /<script(?![^>]*\bsrc=)[^>]*>([\s\S]*?)<\/script>/gi;

let match;
let source = "";
while ((match = scriptPattern.exec(html)) !== null) {
  source += `${match[1]}\n`;
}

if (!source.trim()) {
  throw new Error("No inline JavaScript found in index.html.");
}

// Parsing via Function checks syntax without executing the application code.
new Function(source);
console.log("index.html inline JavaScript syntax OK");
