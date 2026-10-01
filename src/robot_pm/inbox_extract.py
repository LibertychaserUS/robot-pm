"""Extract text for inbox import. Headings become sections. No OCR."""

from __future__ import annotations

import json
import sys
from pathlib import Path

from docx import Document
from markdown_it import MarkdownIt
from pypdf import PdfReader


def extract_file(path: Path) -> dict:
    suffix = path.suffix.lower()
    if suffix == ".md":
        return _from_sections(_markdown_sections(path.read_text(encoding="utf-8")))
    if suffix == ".docx":
        return _from_sections(_docx_sections(path))
    if suffix == ".pdf":
        return _from_pdf(path)
    return {"kind": "unrecognized"}


def _from_sections(sections: list[dict]) -> dict:
    lines: list[str] = []
    for section in sections:
        title = section["title"].strip()
        body = [part.strip() for part in section["body"] if part.strip()]
        if title:
            lines.append(title)
        lines.extend(body)
    text = "\n".join(lines).strip()
    if not text:
        return {"kind": "empty_text"}
    return {"kind": "text", "text": text, "sections": sections}


def _markdown_sections(source: str) -> list[dict]:
    sections: list[dict] = []
    current = {"title": "", "body": []}
    heading = False
    for token in MarkdownIt().parse(source):
        if token.type == "heading_open":
            if current["title"] or current["body"]:
                sections.append(current)
            current = {"title": "", "body": []}
            heading = True
        elif token.type == "inline" and heading:
            current["title"] = token.content
            heading = False
        elif token.type == "inline":
            current["body"].append(token.content)
    if current["title"] or current["body"]:
        sections.append(current)
    return sections


def _docx_sections(path: Path) -> list[dict]:
    document = Document(str(path))
    sections: list[dict] = []
    current = {"title": "", "body": []}
    for paragraph in document.paragraphs:
        style = paragraph.style.name if paragraph.style is not None else ""
        if style.startswith("Heading"):
            if current["title"] or current["body"]:
                sections.append(current)
            current = {"title": paragraph.text, "body": []}
        elif paragraph.text:
            current["body"].append(paragraph.text)
    if current["title"] or current["body"]:
        sections.append(current)
    return sections


def _from_pdf(path: Path) -> dict:
    reader = PdfReader(str(path))
    pages: list[str] = []
    for page in reader.pages:
        pages.append(page.extract_text() or "")
    text = "\n".join(pages).strip()
    if not text:
        return {"kind": "empty_text"}
    return {"kind": "text", "text": text}


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("用法：python -m robot_pm.inbox_extract <文件>", file=sys.stderr)
        return 2
    json.dump(extract_file(Path(argv[1])), sys.stdout, ensure_ascii=False)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
