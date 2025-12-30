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
    echo '```' >> "$WIKI_DIR/Man-Page.md"
    GROFF_NO_SGR=1 groff -man -Tascii -P-c "$MANPAGE" 2>/dev/null | col -bx >> "$WIKI_DIR/Man-Page.md"
    echo '```' >> "$WIKI_DIR/Man-Page.md"
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
# _Sidebar.md - Navigation sidebar
#
echo "Creating _Sidebar.md..."
cat > "$WIKI_DIR/_Sidebar.md" << 'EOF'
## Wiki

- [[Home]]
- [[Man Page]]

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
