# Scenario-based device testing

This guide describes the shared device-testing runner, regression scenarios and
release evidence. Implementation is in progress in this draft pull request.

Physical devices, emulators and optional Android Device Streaming use the same
scenario expectations. Human play and visual review are required alongside
automatic observations. Developer quest shortcuts do not certify release packages.

Device addresses, Firebase projects, fixture paths, credentials, game data and
captures stay local. Keep configuration in ignored `LOCAL_TESTING.md` and
`device-testing.local.json`; results belong under `out/`.
