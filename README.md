# Loop

Loop is a browser voice client and evidence viewer.
Its backend uses C. Its frontend uses TypeScript.
Voice uses WebTransport, starts with a tap, and supports full duplex.
The interface supports mobile browsers with WebTransport.

This source tree preserves the paths of required shared libraries.
It excludes private captures, recordings, model weights, vendor archives, deployment state, and private Git history.
The MIT notices apply to the included source under their original terms.
External dependencies retain their own licenses.

Start with the [installation guide](product/loop/open-source/INSTALL.md).
The guide covers source checks, local image builds, and Authentik integration.
The [contribution guide](product/loop/open-source/CONTRIBUTING.md) defines supported changes.
The [security guide](product/loop/open-source/SECURITY.md) defines private reporting and capture boundaries.
The [dependency notices](product/loop/open-source/THIRD-PARTY-NOTICES.md) identify external license sources.

Authentik, the C voice runtime, and native model engines supply the connected voice path.
Anvil owns model admission, execution, experiments, reviews, and promotion.
Loop adds no model execution controller.
Provider-reported model names do not verify loaded model weights.
Missing usage and cost remain unknown.

This source does not include a trained Lynn voice checkpoint.
Local tests use synthetic protocol fixtures and require no live microphone.
They do not establish phone latency, acoustic acceptance, or production recovery.
