#pragma once

#include <string>
#include <vector>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace codetopo {

// Embedded skill files — compiled into the binary
struct SkillDef {
    const char* name;
    const char* filename;
    const char* description;
    const char* content;
};

// The refactor skill content (embedded at compile time)
static const char* SKILL_REFACTOR_CONTENT = R"SKILL(# Refactor: Decompose a Large Class

When asked to refactor a large file or class, use codetopo MCP tools to analyze and plan the extraction without reading the full file.

## When to use

- File exceeds ~1,000 lines
- Single class with >15 methods
- User asks to "refactor", "split", "decompose", or "break up" a large file

## Step 1: Analyze

Call `dependency_cluster` with the file path:
```
dependency_cluster(path: "path/to/file.ts")
```

This returns every method with:
- **extractability**: 1.0 = pure reader (safe to extract), 0.0 = pure writer (keep in class)
- **clusters**: methods grouped by shared field access
- **shared_fields**: the coupling between clustered methods

For individual method detail, call `method_fields`:
```
method_fields(symbol: "methodName", file: "path/to/file.ts")
```

## Step 2: Plan modules

Group extractable methods (extractability >= 0.8) by cluster into modules. Name by concern:
- Methods sharing render/display fields -> `renderHelpers.ts`
- Methods sharing chain/history fields -> `summary.ts`
- Methods with isolated state (<=3 fields) -> `recording.ts`, `navigation.ts`
- Pure utilities (0 field access) -> already standalone or in `helpers.ts`

## Step 3: Create state interface

Create `types.ts` with an interface listing all class fields plus any class methods that extracted functions need to call back:
```typescript
export interface ClassState {
  fieldA: TypeA;
  // Methods extracted functions call back into
  updateView(): void;
}
```

## Step 4: Extract methods

For each method to extract:

1. Get the body: `context_for(symbol: "methodName", file: "path/to/file.ts")` -> `source` field
2. Transform mechanically:
   - `private [async] methodName(args)` -> `export [async] function methodName(p: ClassState, args)`
   - `this.extractedSibling(args)` -> `extractedSibling(p, args)`
   - `this.pureFunction(args)` -> `pureFunction(args)` (no state param for pure functions)
   - `this.field` -> `p.field`
   - Bare `this` passed as argument -> `p`
3. Write to the module file with appropriate imports

This can be scripted -- the transform is mechanical regex on the source from `context_for`.

## Step 5: Rewrite class as thin shell

The class keeps:
- All field declarations
- Constructor
- Small config getters (< 5 lines)
- One-liner delegations: `getHtml(): string { return getHtml(this); }`

## Step 6: Build and fix

After each phase, compile and fix:
- **Missing cross-module imports**: method A calls method B from another module
- **async return types**: `void` -> `Promise<void>`
- **Variable shadowing**: inner loop variable `s` collides with state parameter
- **Pure functions with extra state param**: `escapeHtml(p, text)` should be `escapeHtml(text)`
- **Template literals**: keep `<style>`/`<script>` tags in the template, extract only content

## Phasing

Extract in phases, compile between each:

| Phase | What | Expected reduction |
|-------|------|-------------------|
| 1 | Pure reader methods (extractability >= 0.8) | 30-50% |
| 2 | Mutation core (big switches, getHtml) as standalone functions | Another 30-50% |
| 3 | Static content (CSS, JS templates, large string constants) | Remaining bloat |

Re-run `dependency_cluster` after each phase to see what's left.

## Warnings

- **Use `p` not `s`** as the state parameter name. Inner loops often use `for (const s of items)` which shadows `s`.
- **Never read the original file directly.** Use `context_for` to get method bodies.
- **Use `symbol` + `file` parameters** instead of `node_id`. Node IDs change when files are re-indexed.
- **Pure utility functions** (0 field access in `method_fields`) should NOT get a state parameter.
- **Class methods called by extracted functions** (like `updateWebview`) must be added to the state interface.
)SKILL";

static const SkillDef AVAILABLE_SKILLS[] = {
    {"refactor", "SKILL.md", "Decompose large classes using dependency_cluster + method_fields analysis", SKILL_REFACTOR_CONTENT},
};
static const int SKILL_COUNT = sizeof(AVAILABLE_SKILLS) / sizeof(AVAILABLE_SKILLS[0]);

// List available skills
inline void skills_list() {
    std::cout << "Available skills:\n\n";
    for (int i = 0; i < SKILL_COUNT; ++i) {
        std::cout << "  " << AVAILABLE_SKILLS[i].name << "\n";
        std::cout << "    " << AVAILABLE_SKILLS[i].description << "\n\n";
    }
}

// Install a skill into the target repo
inline int skills_install(const std::string& skill_name, const std::string& root) {
    // Find the skill
    const SkillDef* skill = nullptr;
    for (int i = 0; i < SKILL_COUNT; ++i) {
        if (AVAILABLE_SKILLS[i].name == skill_name) {
            skill = &AVAILABLE_SKILLS[i];
            break;
        }
    }
    if (!skill) {
        // "all" installs everything
        if (skill_name == "all") {
            for (int i = 0; i < SKILL_COUNT; ++i) {
                skills_install(AVAILABLE_SKILLS[i].name, root);
            }
            return 0;
        }
        std::cerr << "Unknown skill: " << skill_name << "\n";
        std::cerr << "Available: ";
        for (int i = 0; i < SKILL_COUNT; ++i) {
            if (i > 0) std::cerr << ", ";
            std::cerr << AVAILABLE_SKILLS[i].name;
        }
        std::cerr << "\n";
        return 1;
    }

    // Determine install location — prefer .github/copilot-instructions.md appendage,
    // fall back to .codetopo/skills/<name>/SKILL.md
    namespace fs = std::filesystem;
    fs::path skill_dir = fs::path(root) / ".codetopo" / "skills" / skill->name;
    fs::path skill_path = skill_dir / skill->filename;

    fs::create_directories(skill_dir);
    std::ofstream out(skill_path);
    if (!out) {
        std::cerr << "Failed to write " << skill_path.string() << "\n";
        return 1;
    }
    out << skill->content;
    out.close();

    std::cout << "Installed: " << skill_path.string() << "\n";

    // Also append a reference to copilot-instructions.md if it exists
    fs::path instructions = fs::path(root) / ".github" / "copilot-instructions.md";
    if (fs::exists(instructions)) {
        // Check if already referenced
        std::ifstream check(instructions);
        std::string existing((std::istreambuf_iterator<char>(check)),
                              std::istreambuf_iterator<char>());
        check.close();

        std::string ref = ".codetopo/skills/" + std::string(skill->name) + "/" + skill->filename;
        if (existing.find(ref) == std::string::npos) {
            std::ofstream append(instructions, std::ios::app);
            append << "\n\n## Skill: " << skill->name << "\n\n";
            append << "See [" << ref << "](" << ref << ") for the full skill instructions.\n";
            append.close();
            std::cout << "Referenced in: " << instructions.string() << "\n";
        }
    }

    return 0;
}

inline int run_skills(const std::string& action, const std::string& skill_name,
                       const std::string& root) {
    if (action == "list") {
        skills_list();
        return 0;
    }
    if (action == "install") {
        if (skill_name.empty()) {
            std::cerr << "Usage: codetopo skills install <skill-name|all>\n";
            skills_list();
            return 1;
        }
        return skills_install(skill_name, root);
    }
    std::cerr << "Unknown action: " << action << "\n";
    std::cerr << "Usage: codetopo skills <list|install> [skill-name]\n";
    return 1;
}

} // namespace codetopo
