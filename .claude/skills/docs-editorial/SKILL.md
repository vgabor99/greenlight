---
name: docs-editorial
description: "Editorial pass over greenlight's Markdown docs: improve readability, language, narrative flow and coherence, and fix structural blind spots, without changing technical meaning or touching non-Markdown files. Use only when the user asks for docs-editorial by name."
disable-model-invocation: true
---

# Documentation Editorial Improvement

Review and improve the existing Markdown documentation of this codebase.

The documentation is already technically accurate, complete in its intended scope, and reasonably well structured. Do not treat this as an opportunity to redesign, expand, or rewrite the documentation from scratch.

Your task is to improve its **readability, clarity, language, narrative flow and overall coherence**, while identifying and fixing structural blind spots that make the documentation harder to understand or navigate.

Think like an experienced technical writer editing documentation written by an experienced engineer.

## 0. Scratchpad

Maintain a scratchpad file to keep progress and stay focused on what to do. Keep it in Claude's scratchpad
directory, never in the repository (it would be a new file). Record in it:

- The files to review, and the status of each (read, opportunities noted, edited, reviewed).
- The opportunities found per file, each marked as fixed, skipped (with the reason) or open.
- Headings renamed, so links and anchors can be checked at the end.

Re-read it before each step and update it after each file.

## 1. Strict scope restrictions

**ONLY existing Markdown documentation files (`.md`) may be modified.**

- Do not modify source code, configuration files, build scripts, tests or any other non-Markdown files.
- Do not rename, move or delete existing files.
- Do not introduce new dependencies or tooling.
- Do not change the technical meaning of any documentation.
- Do not make implementation changes, even if you discover something that could be improved.
- Preserve existing technical facts, examples, commands, APIs, configuration details and terminology.

These restrictions are absolute.

## 2. Preserve the existing documentation

Treat the existing documentation as the authoritative editorial baseline.

- Preserve its technical content and intended scope.
- Preserve its overall organization and information architecture.
- Do not rewrite content merely for the sake of rewriting it.
- Do not add explanations of concepts that are already adequately covered.
- Do not expand the documentation with generic background information.
- Do not remove technical details simply to make the text shorter.
- Avoid introducing new documents or reorganizing existing ones unless there is a clear, demonstrable benefit to reader comprehension.

The goal is refinement, not reinvention.

## 3. Identify structural blind spots

Look for places where the existing documentation is technically correct but could be easier to understand because of missing context, poor transitions or an insufficiently clear presentation.

Examples include:

- A concept being introduced before the reader has enough context to understand it.
- An explanation that assumes knowledge established only much later.
- An abrupt transition between related topics.
- An important relationship between two components being implicit rather than explained.
- A section that contains correct information but lacks a useful introduction or conclusion.
- A missing connection between related explanations in different documents.
- A heading hierarchy that does not accurately reflect the conceptual hierarchy.
- Information that is individually clear but difficult to understand in its current sequence.
- A missing cross-reference that would help readers navigate related material.

Fix these issues with minimal, targeted changes.

Do not invent structural problems where none exist.

## 4. Improve writing quality

This is the primary objective.

Write for technically competent humans who want to understand the system, not for machines, search engines or documentation checklists.

### Language and style

- Use natural, fluent, precise language.
- Prefer clear explanations over compressed technical statements.
- Prefer connected prose over excessive bullet lists.
- Make paragraphs flow naturally from one idea to the next.
- Use transitions where they help establish relationships between ideas.
- Avoid unnecessarily long or convoluted sentences.
- Avoid fragmented, telegraphic writing.
- Avoid repetitive explanations and redundant wording.
- Avoid excessive headings and nested lists.
- Avoid generic AI writing patterns, filler phrases and boilerplate.
- Avoid unnecessary introductions, summaries and conclusions.
- Do not make the writing artificially conversational or informal.
- Maintain a consistent, professional engineering tone.

### Explain, don't merely enumerate

Where appropriate, improve explanations so readers understand:

- What something is.
- What purpose it serves.
- How it relates to surrounding concepts.
- Why a particular distinction or behavior matters.

Do not force all four into every explanation. Include only what genuinely helps the reader.

### Narrative flow

Make the documentation read as a coherent explanation rather than a collection of independent facts.

- Establish context before introducing detail.
- Move from general concepts to specifics where appropriate.
- Make dependencies between ideas explicit.
- Ensure paragraphs connect naturally.
- Avoid making readers reconstruct relationships that could be explained directly.

### This repository's docs rules

The docs rules in CLAUDE.md still apply and win over the style guidance above where they conflict:

- Readers may not be native English speakers: simple grammar, clarity over style. Fluent does not mean
  literary; keep sentences short and plain.
- Each fact stays in one place; link to it instead of repeating it.
- English first: screens by their names in docs/uispec/ui-spec.md, in italics; Hungarian only in parentheses
  after the English. docs/uispec/ui-spec.md is exempt.
- Mermaid diagrams stay vertical (no `direction LR`).

## 5. Maintain technical fidelity

Although the task is editorial, technical accuracy must remain unchanged.

- Do not alter the meaning of technical statements.
- Do not reinterpret design decisions.
- Do not introduce assumptions or undocumented intentions.
- Preserve exact identifiers, API names, commands, paths and configuration keys.
- Preserve the meaning of examples and code snippets.
- Do not simplify explanations to the point where important technical distinctions disappear.

If a sentence is ambiguous, improve its wording without changing its technical meaning.

If you cannot confidently improve it without risking a change in meaning, leave it unchanged.

## 6. Editing approach

Work in the following order:

**First — Read and understand**

Read the existing Markdown documentation as a connected whole. Understand its organization, intended audience, terminology and narrative. Start at docs/README.md, which says which file answers what.

Do not begin editing immediately.

**Second — Identify opportunities**

Identify:
- Awkward or unnatural language.
- Unnecessarily complex sentences.
- Poor paragraph structure.
- Repetitions and redundancy.
- Unclear explanations.
- Weak transitions.
- Structural blind spots.
- Inconsistent writing style.

Distinguish genuine problems from stylistic preferences. Not every sentence needs improvement.

**Third — Edit**

Make focused, purposeful improvements.

Prefer small, precise changes over wholesale rewrites. Preserve good existing writing.

Only change document structure when doing so clearly improves comprehension.

Headings are link targets: code comments, skills and other docs point to them as `docs/<file>.md#<heading>`.
Rename a heading only when it clearly helps, and note it in the scratchpad.

**Fourth — Review**

Read the resulting documentation from the perspective of someone encountering the project for the first time.

Check:
- Is the language natural and easy to follow?
- Does each explanation provide enough context?
- Does the narrative progress logically?
- Are relationships between concepts clear?
- Is terminology consistent?
- Does the documentation retain its original technical meaning?
- Have any unnecessary changes been introduced?

## 7. Final verification

Inspect the Git diff.

- Every modified file must be an existing Markdown file.
- No non-Markdown files may have been modified.
- No existing user changes may be overwritten or reverted.
- Avoid unrelated formatting changes.
- Ensure that the diff reflects meaningful editorial improvements rather than cosmetic churn.

Run `python tools/check_docs.py` (the mechanical pass of the `docs-check` skill) to catch links and anchors
broken by the edits. A link from a non-Markdown file to a renamed heading cannot be fixed here: restore the
heading instead.

## 8. Final report

Provide a concise summary of:
- The Markdown files modified.
- The most significant improvements in clarity, language and flow.
- Any structural blind spots that were addressed.
- Confirmation that no non-Markdown files were modified.

Do not claim checks that were not actually performed.

**Guiding principle: Improve how the existing documentation communicates, not what the documentation says. Preserve what is already good. Change only what makes the reader's experience genuinely better.**
