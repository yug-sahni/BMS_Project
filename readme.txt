# TASK: Build Version 1 of the Project Using the Provided Word Document "Mini Project - Banking Management System.docx"

You are acting as a senior software engineer and full-stack developer. Your task is to develop **Version 1 (V1) of this project, based exclusively on the project specification provided in the Word document located in the current project folder.**

## 1. Read and Understand the Documentation First

Before writing or modifying any code:

1. Inspect the current working directory and identify the Word document (`.docx` or `.doc`).
2. Read and analyze the entire document, including all sections, requirements, features, workflows, constraints, tables, and diagrams that can be accessed.
3. Treat this document as the **single source of truth** for the project.
4. Extract the functional requirements, non-functional requirements, user roles, system workflows, inputs, outputs, business rules, and technical constraints explicitly stated in the document.
5. Inspect the existing project files, if any, to understand the current implementation before making changes.

Do not begin implementation until you understand the documented requirements.

## 2. Strict Scope: Use Only the Word Document

This is the most important rule.

- Implement only features, functionality, and behavior explicitly required or clearly implied by the Word document.
- Do not invent features, add unnecessary functionality, or expand the project scope.
- Do not use external project descriptions, online templates, unrelated codebases, or assumptions about what the project should do.
- Do not add features merely because they are common in similar applications.
- If a requirement is ambiguous, record the ambiguity and choose the simplest implementation consistent with the document. If it could materially change the architecture or behavior, ask me before proceeding.
- Do not silently invent missing business rules, APIs, credentials, datasets, or integrations.
- Use sensible engineering defaults only for implementation details that the document does not specify, and document those choices.

**The objective is to implement the project described in the document, not a different or more ambitious version of it.**

## 3. Create a Version 1 Implementation Plan

After reading the document, create a concise implementation plan containing:

1. Project objective.
2. Requirements extracted from the document.
3. Features included in V1.
4. Technology stack specified in the document.
5. Proposed project architecture and folder structure.
6. Database schema and relationships, if applicable.
7. APIs, interfaces, screens, or modules required.
8. Dependencies and setup requirements.
9. Testing strategy.
10. Ambiguities, missing information, and assumptions.

Separate explicitly documented requirements from implementation assumptions.

Save this plan as `V1_IMPLEMENTATION_PLAN.md`.

Do not add features outside the documented scope. If the document leaves a technology choice open, select the simplest suitable option and explain the choice in the plan.

## 4. Implement the Project

Once the plan is ready, implement Version 1 in the current project directory.

Follow these principles:

- Follow the technology stack and constraints stated in the document.
- Use clean, modular, maintainable, and readable code.
- Implement the complete V1 scope rather than producing only a mockup or superficial prototype.
- Connect the required frontend, backend, database, and external services wherever the document requires them.
- Implement the documented validation, error handling, and business logic.
- Create the required database tables, schemas, migrations, or initialization scripts where applicable.
- Use configuration files and environment variables for secrets instead of hardcoding credentials.
- Provide example environment configuration files when needed, without including real secrets.
- Include only dependencies needed for the documented requirements.
- Do not replace required functionality with hardcoded demo data or placeholder buttons.
- Do not overwrite existing work unnecessarily. Inspect files before modifying them.
- Never delete unrelated files or perform destructive operations without asking me first.
- Do not claim that a feature works unless you have implemented and tested it.

If the document explicitly requires AI/ML functionality, implement the documented AI/ML workflow. Do not introduce unrelated AI models, services, or capabilities.

## 5. Verify the Implementation

After implementing the project:

1. Install or identify the required dependencies.
2. Run the available compiler, build process, tests, and static checks.
3. Test the major documented user workflows.
4. Verify database connectivity and API integration when applicable.
5. Fix errors that arise from your implementation.
6. Identify anything that could not be tested because of missing credentials, services, hardware, or other environmental limitations.
7. Check that the final implementation matches the Word document and does not contain unrequested features.

Do not claim tests passed if you did not run them.

Do not run destructive commands or overwrite important data without my approval.

## 6. Deliverables

At the end, ensure the project contains the following where applicable:

- The working V1 source code.
- `V1_IMPLEMENTATION_PLAN.md`
- `README.md` containing setup, configuration, execution, and testing instructions.
- Required dependency manifests.
- Database initialization or migration scripts.
- Tests for the important documented requirements.
- `.env.example` if environment variables are required.
- A final report named `V1_IMPLEMENTATION_REPORT.md` listing:
  - Features implemented.
  - Corresponding requirements from the Word document.
  - Files created or modified.
  - Commands used to run the project.
  - Tests executed and their actual results.
  - Known limitations.
  - Requirements not implemented and the reason for each.

Do not create unnecessary documentation or files that do not help deliver V1.

## 7. Execution Instructions

Work autonomously through the implementation, testing, and correction phases.

- Do not stop after merely summarizing the document or creating a plan.
- After preparing the plan, proceed with implementation unless a critical ambiguity or destructive action requires my approval.
- If you encounter a minor issue, use the simplest reasonable solution consistent with the specification.
- If you encounter a blocking issue, explain exactly what is missing and ask a focused question.
- Keep the implementation within the documented V1 scope.
- Make sure the final project can be run by following the README instructions.

## 8. Final Acceptance Criteria

Version 1 is complete only when:

1. The Word document has been read and analyzed.
2. Every feasible, in-scope requirement has been implemented.
3. The project builds or runs to the extent supported by the environment.
4. The documented workflows have been tested to the extent possible.
5. The setup and execution instructions are provided.
6. Known gaps and limitations are reported honestly.
7. No unrequested product features have been added.

**Start now:** Inspect the current directory, locate and read the Word document in full, prepare `V1_IMPLEMENTATION_PLAN.md`, and then build, test, and document Version 1 strictly according to that document.
