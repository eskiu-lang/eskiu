# Stability

Eskiu is stable since 0.13: code that compiles with 0.13.0 keeps compiling in every 0.13.x
release. Before 1.0 a minor release (0.13, 0.14) can still change the language or the
standard library, but every change that can break a program follows the rules below, so code
moves forward with warnings instead of failing to compile.

## Standard library names

- A public name (one without a leading `_`) is not renamed or removed in a patch release
  (0.13.0 to 0.13.1).
- A rename in a minor release keeps the old name for that release as a wrapper marked
  `deprecated("use NEW")`. Each use prints a warning that names the replacement; the next
  minor release removes it. `-Wno-deprecated` silences the warnings.
- The CHANGELOG lists every renamed or removed name with its replacement.
- Names that start with `_` are internal. They can change in any release.

## The language

- A change that makes a valid program invalid (a new error, a new keyword, a different
  meaning) lands in a minor release and is listed in the CHANGELOG under "Changed", with
  what to edit. When the old form can be detected, the release before warns about it first.
- New syntax uses contextual words where it can (`deprecated` is not a keyword), so existing
  names keep working.
- Patch releases only fix bugs. A fix can still reject a program the compiler accepted by
  mistake (for example code that miscompiled); the CHANGELOG says so.

## `deprecated`

A function marked deprecated still works:

```eskiu
int Rect_area(const Rect* self) { return self.w * self.h; }
deprecated("use Rect_area") int rect_area(const Rect* r) { return Rect_area(r); }
```

A call to `rect_area` prints `warning: 'rect_area' is deprecated: use Rect_area`, at the
call. Uses inside an imported module are not reported, so a library can keep a wrapper that
calls another deprecated name without warning its users.

## What is stable

The C ABI of `extern` functions, the object files and the command-line flags of `eskiuc`
documented in `eskiuc --help` follow the same rules as the standard library.
