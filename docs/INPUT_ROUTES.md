# Private input routes

The runtime admits PSXRTI1 digital, PSXRTI2 DualShock and PSXRTI3 routes.
An admitted route owns its declared connection and controller type during
desktop device refresh. Host controller handles still refresh. Physical
sampling does not replace admitted RTI2 input with the configured local seat.
Invalid routes are refused and do not own the ports.

In a diagnostic build, set `PSX_INPUT_ROUTE_RECORD` to record P1 from boot.
During that recording, Shift+F11 requests a MENU marker and Shift+F12 requests
a GAMEPLAY marker at the next frame boundary. Plain F11 remains fullscreen;
plain F12 starts or stops a player replay. Recording and replay acceptance
remain specific to the exact product and route identities.

The route-session regression covers admitted/refused RTI2 and the old narrow
ownership defect. The desktop-refresh regression executes the production
refresh body with authored host seams. These checks do not establish a
visible replay or a game's controller acceptance.
