# README screenshots

These images are the curated public examples used by the repository's top-level
[`README.md`](../../README.md). They are not lab evidence; measured comparisons and debug
visualizations belong in [`docs/img/`](../img/) and the relevant design note.

## Current set

| File | Scene and purpose | Capture notes |
|---|---|---|
| `bright-falls-ferry.jpg` | Bright Falls ferry; general hero image and ray-traced AO on a moving platform | Torchbearer 0.6.9 development build, `awr-brightfalls-day`, 1920×1080 capture resized to 1600×900; `ao = rtao`, instance motion on |
| `transformer-yard-flashlight.jpg` | Night flashlight scene in the transformer yard | Torchbearer 0.6.9 development build, `awr-forest-night`, 1920×1080 capture resized to 1600×900; RTAO and ray-traced flashlight path enabled |
| `stereoscopic-3d.jpg` | Half side-by-side stereo pair with the flashlight aimed | Torchbearer 0.6.9 development build, `awr-forest-night`, 1920×1080 capture resized to 1600×900; `mode = sbs`, `output = hsbs`, `skip_shadows = 1` |
| `first-person-close-framing.jpg` | Experimental first-person close framing | Existing documentation capture copied from `docs/img/fp-close-aim-merged.jpg`; recapture this first when first-person visuals next change |

## Update policy

Refresh a screenshot in the same change that materially alters the pictured feature, its
defaults, its visible quality, or the release it claims to represent. At minimum, review the
whole set for every public release.

When replacing an image:

1. Capture from a clean current build, not a debug visualization, unless the README caption
   explicitly says it is a debug view.
2. Use a reproducible named scene or fixture and record it in the table above.
3. Record the mod version and the settings that matter to the image.
4. Export a JPEG no wider than 1600 px and preferably under 400 KB; strip metadata.
5. Keep descriptive filenames stable unless the subject changes.
6. Update the README alt text and caption if the subject or settings changed.
7. Check the rendered README at desktop and mobile widths before publishing.

Do not silently reuse an old screenshot to illustrate a changed visual feature. If an image is
known to predate the current release, say so in this file until it is replaced.
