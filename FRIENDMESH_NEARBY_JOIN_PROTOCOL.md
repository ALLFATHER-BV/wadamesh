# FriendMesh nearby direct-join protocol

Status: simple existing-MeshCore channel join implemented and build-verified;
the LoRa direct-join bridge and Bluetooth discovery have been physically verified
between two T-Decks. Production FriendMesh handshake/rekey security remains open.
Bounded roster, join acknowledgement, leave, and cooperative removal behavior
now build but also require their first multi-device test.

## Purpose

Nearby direct join is the preferred FriendMesh group-onboarding experience. Two
people with their devices together should be able to join a private group by
comparing a short number and approving the request. Nobody types, scans, exports,
or transmits an identity private key.

The currently implemented WadaMesh experience is:

```text
Chats -> create/open private channel -> channel actions -> Invite nearby
      -> scan for nearby saved contacts -> choose contact -> encrypted direct invite
      -> recipient reviews Join prompt -> channel appears in normal Chats
```

This uses WadaMesh's existing chat, contact, notification, and modal patterns.
It does not introduce a FriendMesh launcher or replacement chat design.

## Implemented existing-protocol bridge

This first radio-capable slice deliberately reuses existing MeshCore and
WadaMesh behavior instead of creating a second group transport:

- a group chat is an ordinary WadaMesh private `ChannelDetails` channel;
- the invitation carries that channel's name and 16-byte channel secret in a
  compact, bounded `FMCH1` envelope;
- the envelope is sent using MeshCore's existing encrypted direct-message path
  to an existing saved chat contact;
- the sender forces an empty route and requires either a Bluetooth presence
  observation no older than 30 seconds or a zero-hop LoRa advert no older than
  120 seconds;
- Bluetooth presence is a bounded three-second active scan. Its advertisement
  contains only a versioned six-byte public-key prefix so it can be matched to
  an already-saved chat contact; it never contains the channel secret;
- the receiver accepts the envelope only when MeshCore reports a direct packet
  with zero path hashes;
- the secret is not shown as chat text and is wiped from temporary invite
  buffers after use;
- the recipient must press `Join` before the channel is saved.
- acceptance sends a compact encrypted direct acknowledgement to the inviter;
- a bounded eight-member roster records invitation and membership states in the
  existing blob store and synchronizes as an encrypted MeshCore group-data
  control record, never as channel text;
- guests can send an encrypted leave notice and remove the channel locally;
- an owner can send a pairwise encrypted cooperative removal notice to an
  updated member device.

The channel secret is a symmetric group secret, not anyone's identity private
key. This bridge has no plaintext fallback. Duplicate channels, name conflicts,
invalid envelopes, full channel storage, non-chat contacts, already-joined
members, stale observations, and relayed packets are rejected.

Roster synchronization is functional metadata, not an authorization protocol.
Anyone who already possesses the shared channel key can still receive traffic
and can forge a channel-level roster snapshot. The implementation pins snapshots
to the administrator learned through the direct join and rejects snapshots that
omit the local active member, but only authenticated membership events and key
rotation can complete this boundary. A leave or removal therefore sets
`rekey required` and must not be described as cryptographic revocation.

## Security boundary

`0 hops` means the reviewed receive path observed a direct packet with FriendMesh
forwarding disabled. It does not prove physical distance, identity,
confidentiality, or exclusive reception. LoRa may cover long distances, passive
listeners can receive direct packets, and a malicious external relay can create
wormhole-like behavior. Bluetooth RSSI is also not a distance or authentication
proof. The public-key prefix is observable presence metadata and may permit
tracking until a future privacy design replaces it with a rotating identifier.

For the simple implemented bridge, direct-path evidence, an existing encrypted
MeshCore contact session, and explicit recipient confirmation are the enforced
gates. For the later production FriendMesh protocol, direct evidence remains
only one gate and a join additionally requires:

1. an unexpired invitation;
2. a fresh direct exchange with forwarding disabled;
3. identity-reference agreement;
4. a matching short-authentication comparison or verified QR transcript;
5. explicit administrator approval;
6. an authenticated, member-specific encrypted group-key grant.

## User flow

### Create the group

1. The creator selects `New` and `Private group` from the existing Chats screen.
2. They enter the group name, their alias, and optional group policies.
3. The device creates a random opaque group ID, membership epoch, administrator
   record, and—after production security exists—a random group epoch key.
4. The group appears in the normal WadaMesh chat list.

### Open a nearby invitation

1. The administrator opens group details and selects `Invite nearby`.
2. The device creates a short-lived invitation with a random invitation ID and
   challenge.
3. The screen displays a QR code and a six-character convenience code.
4. FriendMesh accepts the join exchange only through the reviewed direct receive
   path while the invitation is open.

The short code locates an invitation; it is not a password, membership grant, or
group key. The QR payload may carry a longer random token and public transcript
data, but never private identity material or a plaintext group key.

### Request and verify

1. The joining device scans the QR code, enters the code, or discovers the open
   invitation directly.
2. Both devices exchange fresh public ephemeral references, nonces, and their
   public FriendMesh/MeshCore identity references without forwarding.
3. Both devices derive the same transcript and display a short comparison number.
4. The people compare the number in person. A verified existing FriendMesh
   contact may show continuity information, but the administrator still approves
   the group membership action.
5. The joining person selects an alias and submits a signed join request.

### Approve and secure the new epoch

1. The administrator reviews the contact, aliases, public fingerprints,
   comparison result, invitation age, and requested policies.
2. Approval records a signed membership event and advances the membership epoch.
3. A fresh group epoch key is created so the new member does not automatically
   gain access to earlier encrypted history.
4. Each approved member receives a separately addressed encrypted grant for the
   new epoch. The new member's grant is carried over the authenticated pairwise
   session created by the direct exchange.
5. The new member verifies the administrator authorization, decrypts the grant,
   stores it through protected storage, and returns a receipt.
6. Temporary handshake secrets are erased. Offline existing members remain
   `grant pending`; excluded or removed identities receive no new grant.

The UI should distinguish `request pending`, `approved`, `securing group`,
`waiting for key grant`, and `ready`. Approval alone must not falsely display a
secure ready state.

## Key handling

Each device owns private identity and key-agreement material. Those private keys
never leave the device. Devices exchange public keys and fingerprints.

The only group secret delivered during a join is the new symmetric group epoch
key, inside an authenticated encrypted envelope addressed to one approved member.
The grant binds at least:

- protocol version and cipher-suite identifier;
- group ID and membership epoch;
- invitation and direct-exchange IDs;
- recipient identity and public-key fingerprint;
- administrator identity and administrative sequence;
- role and authorized policies;
- creation/expiration metadata;
- the new group epoch key.

The production suite must be selected and reviewed in the shared security phase.
It requires ephemeral key agreement, a transcript-bound KDF, authenticated
encryption, administrator signatures, nonce discipline, replay protection, test
vectors, and secret wiping. This document does not approve a cipher suite merely
by naming one.

## Required rejection behavior

Reject or keep pending when:

- the observed hop count is not zero;
- forwarding was allowed or direct-path provenance is unavailable;
- the direct observation is stale or predates the invitation;
- the invitation, request, or challenge expired;
- public identity references changed during the exchange;
- comparison numbers do not match;
- the request is duplicated, replayed, blocked, or already a member;
- the administrator rejects or never approves it;
- the group is already in an unresolved rekey transaction;
- the encrypted grant is addressed to another identity or epoch;
- signature, transcript, AEAD, sequence, or replay validation fails;
- protected storage cannot safely commit the grant.

There is no plaintext fallback.

## Functional policy model and future protocol

`FriendMeshMembership` now models:

- `DirectOnly` as the secure-default invitation path policy;
- an explicit alternative path that still requires administrator approval;
- fresh direct-path observations with exchange and ephemeral public references;
- zero observed hops and forwarding-disabled requirements;
- bounded observation age;
- continued verification-transcript and administrator-approval requirements.

The live adapter now uses the pinned MeshCore packet API's direct-route and path
hash count on receive, and an empty route plus either recent Bluetooth presence
or a recent zero-hop advert on send. The invite and Bluetooth-presence codecs are
host-tested and the complete T-Deck firmware builds. The LoRa bridge and later
Bluetooth discovery were both verified between two physical T-Decks.

The same host suite covers roster bounds, persistence framing, malformed roster
records, and join/leave/removal control-envelope framing. Existing groups created
before roster support can be migrated by inviting the same saved member again;
the recipient refreshes membership metadata instead of duplicating the channel.

Phase 7 still must implement protected identity, transcript authentication,
replay defense, protected key storage, member-specific grants, epoch rotation,
and recovery. The implemented channel-secret bridge must not be described as
providing those stronger guarantees.
