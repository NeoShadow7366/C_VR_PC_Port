# Steam Art — Source Image Credits

> **⚠ Status: legacy / to-be-replaced.**
>
> The five JPEGs in this directory were imported early in the project's
> life with **no recorded source, author, or licence**. The filenames
> (`7L14i.jpg`, `GP5cj.jpg`, `et3Ce.jpg`, `tOPhD.jpg`, `jXIxZ.jpg`) are
> Imgur-style random IDs, which strongly suggests they were downloaded
> from a third-party image host. At least one of them
> (`et3Ce.jpg`) additionally has the **Steam logo** baked into the
> artwork, which conflicts with
> [Valve's Steam Brand Guidelines](https://partner.steamgames.com/doc/marketing/branding).
>
> Until each image's provenance and licence can be verified — or the
> images are replaced with art the project owns outright — these source
> files and any artwork derived from them (`../out/qt/`, `../out/vr/`)
> should be treated as **not safe for redistribution** and must not be
> shipped in a release.
>
> The repository's `README.md` therefore intentionally does **not**
> reference any image from `../out/`, and the Windows installer should
> not be considered release-ready until this is resolved.

## To-do

- Replace each source image with art that is either:
  - Original work by the maintainer.
  - AI-generated from a documented prompt with a model whose terms
    permit redistribution under GPL-3.0-or-later.
  - Third-party art whose author, source URL, and licence are recorded
    in this file.
- Remove any embedded Steam, Nintendo, or other third-party trademarks
  from the artwork itself.
- Regenerate `../out/qt/*` and `../out/vr/*` via `../build_art.ps1`.

| File | Used for | Provenance | Action |
| --- | --- | --- | --- |
| `7L14i.jpg` | icon | unknown | replace |
| `GP5cj.jpg` | vertical capsule | unknown | replace |
| `et3Ce.jpg` | VR hero (contains Steam logo) | unknown | replace — trademark issue |
| `tOPhD.jpg` | Qt hero | unknown | replace |
| `jXIxZ.jpg` | logo background | unknown | replace |
