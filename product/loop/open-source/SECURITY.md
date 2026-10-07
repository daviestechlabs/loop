# Security and private evidence

Loop's connected demo requires authenticated admission.
OIDC group checks do not supply load quotas.
Set user and service limits before opening public enrollment.
Keep the HTTP listener behind the authenticated gateway.
Use trusted TLS for browser HTTPS and WebTransport.

Recording is optional and requires the disclosed consent.
Captured model requests can contain complete private context.
Never attach those captures, browser sessions, or signing keys to public issues.
Owner isolation and evidence hashes remain required for readback and recovery.

For a vulnerability, use the hosting repository's private security reporting feature when available.
If it is unavailable, request a private contact without posting exploit details or sensitive data.
Public issue reports should contain only synthetic reproductions.
No independent security certification or support period is claimed.

Historical signing-key verification still depends on the current configured key.
Changing that key can make old capture signatures unverifiable.
Plan key retention and rotation before operating a public evidence service.
