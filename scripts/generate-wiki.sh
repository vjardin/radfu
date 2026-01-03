#!/bin/sh
# Generate GitHub wiki pages from README.md and man page
# No content duplication - extracts/converts from source files
#
# Requires: groff (for man page conversion)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
WIKI_DIR="${1:-$PROJECT_DIR/wiki}"
README="$PROJECT_DIR/README.md"
MANPAGE="$PROJECT_DIR/build/radfu.1"
CONTRIBUTING="$PROJECT_DIR/CONTRIBUTING.md"
LEGAL="$PROJECT_DIR/LEGAL.md"
HARDWARE="$PROJECT_DIR/HARDWARE.md"

mkdir -p "$WIKI_DIR"

echo "Generating wiki pages in $WIKI_DIR"

#
# Home.md - Main README content with wiki navigation header
#
echo "Creating Home.md from README.md..."
cat > "$WIKI_DIR/Home.md" << 'EOF'
> **Wiki Navigation:** [[Man Page]] | [[README Source]](https://github.com/vjardin/radfu/blob/master/README.md)

---

EOF
cat "$README" >> "$WIKI_DIR/Home.md"

#
# Man-Page.md - Convert man page to readable format
#
echo "Creating Man-Page.md..."
cat > "$WIKI_DIR/Man-Page.md" << 'EOF'
# radfu(1) Manual Page

> This page is auto-generated from the radfu.1 man page.
> After installation, also available via: `man radfu`

---

EOF

if [ -f "$MANPAGE" ]; then
    # Convert man page to plain text, strip all formatting
    {
        echo '```'
        GROFF_NO_SGR=1 groff -man -Tascii -P-c "$MANPAGE" 2>/dev/null | col -bx
        echo '```'
    } >> "$WIKI_DIR/Man-Page.md"
else
    cat >> "$WIKI_DIR/Man-Page.md" << 'EOF'
**Man page not found.** Build the project first:

```sh
meson setup build
meson compile -C build
```

Then re-run this script.
EOF
fi

#
# Contributing.md - Contribution guidelines
#
echo "Creating Contributing.md..."
cat > "$WIKI_DIR/Contributing.md" << 'EOF'
> **Wiki Navigation:** [[Home]] | [[Man Page]] | [[Legal]] | [[Source]](https://github.com/vjardin/radfu/blob/master/CONTRIBUTING.md)

---

EOF
cat "$CONTRIBUTING" >> "$WIKI_DIR/Contributing.md"

#
# Legal.md - Legal information
#
echo "Creating Legal.md..."
cat > "$WIKI_DIR/Legal.md" << 'EOF'
> **Wiki Navigation:** [[Home]] | [[Man Page]] | [[Contributing]] | [[Source]](https://github.com/vjardin/radfu/blob/master/LEGAL.md)

---

EOF
cat "$LEGAL" >> "$WIKI_DIR/Legal.md"

#
# Hardware.md - Board layout and wiring
#
echo "Creating Hardware.md..."
cat > "$WIKI_DIR/Hardware.md" << 'EOF'
> **Wiki Navigation:** [[Home]] | [[Man Page]] | [[Contributing]] | [[Legal]] | [[Source]](https://github.com/vjardin/radfu/blob/master/HARDWARE.md)

---

EOF
cat "$HARDWARE" >> "$WIKI_DIR/Hardware.md"

#
# _Sidebar.md - Navigation sidebar
#
echo "Creating _Sidebar.md..."
cat > "$WIKI_DIR/_Sidebar.md" << 'EOF'
## Wiki

- [[Home]]
- [[Man Page]]
- [[Hardware]]
- [[Contributing]]
- [[Legal]]

## Links

- [GitHub](https://github.com/vjardin/radfu)
- [Issues](https://github.com/vjardin/radfu/issues)
- [Releases](https://github.com/vjardin/radfu/releases)
EOF

#
# _Footer.md - Common footer
#
echo "Creating _Footer.md..."
cat > "$WIKI_DIR/_Footer.md" << 'EOF'
---
*Auto-generated from [README.md](https://github.com/vjardin/radfu/blob/master/README.md) and man page.*
EOF

echo ""
echo "Wiki pages generated:"
ls -1 "$WIKI_DIR"
