# Steam Art — Source Image Credits

> **Status: clean / self-contained.**
>
> The art pipeline no longer depends on any third-party source imagery.
> [`../build_art.ps1`](../build_art.ps1) synthesises its own background
> bitmaps procedurally (linear gradient + faint scan-lines + simple
> geometric accent), then composites the project title text on top via
> the in-script `Add-TitleBand` / `Build-Logo` helpers.
>
> The synthesised intermediates are written next to this file with a
> `_gen_` prefix; they are regenerated on every `build_art.ps1` run and
> are not checked in (see [`.gitignore`](../../../.gitignore)).
>
> All artwork in `../out/qt/` and `../out/vr/` is therefore wholly the
> work of this project and is licensed under the same terms as the rest
> of the repository (GPL-3.0-or-later).

## History

This file previously documented five third-party JPEGs of unknown
provenance (`7L14i.jpg`, `GP5cj.jpg`, `et3Ce.jpg`, `tOPhD.jpg`,
`jXIxZ.jpg`) that were used as backgrounds, one of which had the Steam
logo baked into the artwork. Those files were removed and the pipeline
was rewritten to be fully self-contained for licensing reasons.

If you want to ship more polished art in place of the procedural
placeholders, add your own files here, record their author / source /
licence in this file, and adjust `build_art.ps1` to use them.
