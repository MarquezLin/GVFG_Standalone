#!/usr/bin/env python3
"""Convert the GVFG Markdown reference to a branded, UPDF-editable PDF."""

from __future__ import annotations

import argparse
import base64
import html
import os
import re
import shutil
import subprocess
import tempfile
import time
import zipfile
from pathlib import Path

CSS = r"""
@page { size: A4; margin: 0 0 17mm; }
* { box-sizing: border-box; }
body { margin: 0; color: #172033; font-family: "Microsoft JhengHei", "Segoe UI", sans-serif; font-size: 9.4pt; line-height: 1.48; }
.page-layout { width: 100%; border: 0; border-collapse: collapse; margin: 0; padding: 0; }
.page-layout > thead { display: table-header-group; }
.page-layout > thead > tr.banner-row > td { width: 100%; height: 25.03mm; padding: 0; border: 0; background: #000; }
.page-layout > thead > tr.header-spacer > td { width: 100%; height: 6mm; padding: 0; border: 0; background: #fff; }
.page-layout > thead img { display: block; width: 210mm; height: 25.03mm; object-fit: fill; }
.page-layout > tbody > tr > td { padding: 0 15mm; border: 0; vertical-align: top; }
.content { position: relative; z-index: 1; }
h1 { color: #0b3a69; font-size: 25pt; margin: 0 0 16pt; padding-bottom: 8pt; border-bottom: 3px solid #2e75b6; }
h2 { color: #0b3a69; font-size: 17pt; margin: 20pt 0 8pt; padding-bottom: 4pt; border-bottom: 1px solid #9fbad2; break-after: avoid; }
h3 { color: #174f7c; font-size: 12.5pt; margin: 15pt 0 6pt; break-after: avoid; }
p { margin: 4pt 0 8pt; }
ul, ol { margin: 4pt 0 8pt 20pt; padding: 0; }
li { margin: 2pt 0; }
code { font-family: Consolas, "Courier New", monospace; font-size: 8.6pt; color: #7a1f45; background: #f3f5f7; padding: .5pt 2.5pt; border-radius: 2px; }
pre { background: #f3f5f7; border-left: 3px solid #2e75b6; padding: 7pt 9pt; white-space: pre-wrap; overflow-wrap: anywhere; break-inside: avoid; }
pre code { color: #172033; background: transparent; padding: 0; }
.content table { width: 100%; border-collapse: collapse; margin: 6pt 0 11pt; font-size: 8.2pt; break-inside: auto; }
.content thead { display: table-header-group; }
.content tr { break-inside: avoid; }
.content th { color: white; background: #2e75b6; text-align: left; font-weight: 600; }
.content th, .content td { border: 1px solid #b7c7d6; padding: 4pt 5pt; vertical-align: top; overflow-wrap: anywhere; }
.content tbody tr:nth-child(even) { background: rgba(245,248,251,.92); }
a { color: #155b91; text-decoration: none; }
.toc { columns: 2; column-gap: 22pt; list-style: none; margin: 0 0 16pt; padding: 10pt 12pt; background: rgba(244,247,250,.92); border: 1px solid #c8d5e1; }
.toc li { break-inside: avoid; margin: 2pt 0; }
.toc-3 { padding-left: 10pt; font-size: 8.7pt; }
.toc-page { break-after: page; page-break-after: always; }
"""


def first_existing(candidates: list[Path]) -> Path | None:
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    return None


def find_pwsh() -> Path:
    found = shutil.which("pwsh.exe")
    candidates = []
    if found:
        candidates.append(Path(found))
    candidates.append(
        Path.home()
        / ".cache/codex-runtimes/codex-primary-runtime/dependencies/native/powershell/pwsh.exe"
    )
    result = first_existing(candidates)
    if not result:
        raise RuntimeError("PowerShell 7 (pwsh.exe) was not found; Markdown conversion needs ConvertFrom-Markdown.")
    return result


def find_edge() -> Path:
    candidates = []
    for variable in ("ProgramFiles(x86)", "ProgramFiles"):
        root = os.environ.get(variable)
        if root:
            candidates.append(Path(root) / "Microsoft/Edge/Application/msedge.exe")
    result = first_existing(candidates)
    if not result:
        raise RuntimeError("Microsoft Edge was not found.")
    return result


def markdown_to_html(markdown: str, pwsh: Path) -> str:
    command = (
        "$ErrorActionPreference='Stop';"
        "[Console]::InputEncoding=[Text.UTF8Encoding]::new($false);"
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false);"
        "$OutputEncoding=[Text.UTF8Encoding]::new($false);"
        "$source=[Console]::In.ReadToEnd();"
        "(ConvertFrom-Markdown -InputObject $source).Html"
    )
    completed = subprocess.run(
        [str(pwsh), "-NoLogo", "-NoProfile", "-Command", command],
        input=markdown.encode("utf-8"),
        capture_output=True,
        check=False,
        creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
    )
    if completed.returncode:
        details = completed.stderr.decode("utf-8", errors="replace").strip()
        raise RuntimeError(f"Markdown conversion failed:\n{details}")
    return completed.stdout.decode("utf-8")


def add_heading_ids_and_toc(body: str) -> tuple[str, str]:
    items: list[str] = []
    counter = 0
    pattern = re.compile(r'<h([23])\s+id="[^"]*">(.*?)</h\1>', re.DOTALL)

    def replace(match: re.Match[str]) -> str:
        nonlocal counter
        counter += 1
        level = int(match.group(1))
        inner = match.group(2)
        plain = html.unescape(re.sub(r"<[^>]+>", "", inner)).strip()
        section_id = f"section-{counter}"
        items.append(
            f'<li class="toc-{level}"><a href="#{section_id}">{html.escape(plain)}</a></li>'
        )
        return f'<h{level} id="{section_id}">{inner}</h{level}>'

    return pattern.sub(replace, body), os.linesep.join(items)


def extract_banner(template_docx: Path) -> bytes:
    with zipfile.ZipFile(template_docx) as archive:
        try:
            return archive.read("word/media/image1.jpeg")
        except KeyError as error:
            raise RuntimeError("Branding template does not contain word/media/image1.jpeg") from error


def build_pdf(input_markdown: Path, output_pdf: Path, template_docx: Path) -> None:
    for required in (input_markdown, template_docx):
        if not required.is_file():
            raise FileNotFoundError(f"Required file not found: {required}")

    source = input_markdown.read_text(encoding="utf-8")
    source = re.sub(r"(?m)^\[TOC\]\s*$", "", source)
    source = re.sub(r"(?m)^===\s*$", "", source)
    body = markdown_to_html(source, find_pwsh())
    title_match = re.match(r"(?s)^\s*<h1[^>]*>(.*?)</h1>\s*", body)
    if not title_match:
        raise RuntimeError("The Markdown document must start with a level-1 heading (# Title).")
    title_html = title_match.group(1)
    title_text = html.unescape(re.sub(r"<[^>]+>", "", title_html)).strip()
    body = body[title_match.end():]
    body, toc = add_heading_ids_and_toc(body)
    banner = base64.b64encode(extract_banner(template_docx)).decode("ascii")

    document = f"""<!doctype html><html lang="zh-Hant"><head><meta charset="utf-8">
<title>{html.escape(title_text)}</title><style>{CSS}</style></head><body>
<table class="page-layout"><thead><tr class="banner-row"><td><img src="data:image/jpeg;base64,{banner}"></td></tr><tr class="header-spacer"><td></td></tr></thead>
<tbody><tr><td><main class="content"><section class="toc-page"><h1>{title_html}</h1>
<h2>目錄</h2><ul class="toc">{toc}</ul></section>{body}</main></td></tr></tbody></table>
</body></html>"""

    output_pdf.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="gvfg-md-pdf-") as temporary:
        temporary_dir = Path(temporary)
        html_path = temporary_dir / "reference.html"
        plain_pdf = temporary_dir / "reference.pdf"
        html_path.write_text(document, encoding="utf-8")

        edge = find_edge()
        uri = html_path.resolve().as_uri()
        completed = subprocess.run(
            [
                str(edge),
                "--headless",
                "--disable-gpu",
                "--no-pdf-header-footer",
                f"--print-to-pdf={plain_pdf}",
                uri,
            ],
            capture_output=True,
            check=False,
            creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0),
        )
        deadline = time.monotonic() + 15
        while not plain_pdf.is_file() and time.monotonic() < deadline:
            time.sleep(0.2)
        if completed.returncode or not plain_pdf.is_file() or plain_pdf.stat().st_size < 10_000:
            details = completed.stderr.decode(errors="replace").strip()
            raise RuntimeError(f"Microsoft Edge did not create a valid PDF. {details}")

        shutil.copyfile(plain_pdf, output_pdf)
        if not output_pdf.is_file() or output_pdf.stat().st_size < 10_000:
            raise RuntimeError("The generated PDF is missing or unexpectedly small.")


def main() -> None:
    script_dir = Path(__file__).resolve().parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input_markdown", nargs="?", type=Path, default=script_dir / "GVFG_CUSTOMER_API_REFERENCE.md")
    parser.add_argument("output_pdf", nargs="?", type=Path)
    parser.add_argument("template_docx", nargs="?", type=Path, default=script_dir / "GVFG_CUSTOMER_API_REFERENCE.docx")
    args = parser.parse_args()

    input_markdown = args.input_markdown.resolve()
    output_pdf = (args.output_pdf or input_markdown.with_suffix(".pdf")).resolve()
    build_pdf(input_markdown, output_pdf, args.template_docx.resolve())
    print(f"PDF created: {output_pdf}")


if __name__ == "__main__":
    main()
