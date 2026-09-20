# Region discovery and channel scopes

WadaMesh can query nearby MeshCore repeaters for the public regions they allow
and use those names as per-channel flood scopes.

## Discover nearby regions

1. Open **Settings > Radio & Mesh > Known regions**.
2. Select **Scan**.
3. WadaMesh broadcasts one zero-hop node-discovery request, waits for nearby
   repeater replies, then queries each repeater directly for its allowed public
   regions.
4. New names appear under **Discovered**. Select **Add** beside each region you
   want to keep.

The scan queries directly reachable repeaters running a MeshCore version that
supports anonymous region requests. It is bounded by the radio duty-cycle budget
and stops after two minutes if requests cannot be completed.

Region traffic does not expose a reusable region identifier: its transport code
is a payload-dependent HMAC. WadaMesh therefore cannot infer a name by passively
listening. The active scan asks repeaters to provide their configured names.
Private `$region` names are not imported because their keys are not included in
the response and cannot be derived from the name.

## Select a channel scope

1. Open a channel's settings and select **Region & scope**.
2. Choose a saved name from the **Known regions** dropdown. The selection is
   copied into the editable field.
3. Select **Save**.

The field remains editable for manual entry. Leave it blank to use the device's
default region scope. A channel override affects messages sent on that channel;
it does not restrict which messages the device receives.