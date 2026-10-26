# Ad hoc networking

This directory holds Yakumo's ad hoc networking: the client for PSP ad hoc servers, the built-in server behind *Host a Session* and `--adhoc-server`, and the discovery of hosted sessions on a local network.

## License

The files in this directory are licensed under the GNU General Public License, version 3 or (at your option) any later version ([`COPYING`](COPYING)), with the Classpath exception 2.0 ([`EXCEPTION`](EXCEPTION)). The rest of the repository is under the MIT License.

The GPL keeps this code, and future changes to it, open to the projects that share these protocols, such as [aemu](https://github.com/MrColdbird/aemu), [aemu_postoffice](https://github.com/Kethen/aemu_postoffice), [adhoc-ng](https://github.com/friendlyanon/adhoc-ng) and [PPSSPP](https://github.com/hrydgard/ppsspp), so they can use whatever is helpful to them. They may remove the exception when they do; its text allows that.

Without the exception, the GPL would extend to every Yakumo build as a whole and to the mods players load. With it, the rest of Yakumo keeps its MIT terms and mods may use any license, while this directory and every change to it stay under the GPL.

## Protocols

The client and the server speak two protocols designed by others; thanks to that work, Yakumo plays with their servers and clients:

- the adhocctl matchmaking protocol (TCP 27312) from [aemu](https://github.com/MrColdbird/aemu) by MrColdbird;
- the relay protocol (TCP 27313) from [aemu_postoffice](https://github.com/Kethen/aemu_postoffice) by Kethen.

PPSSPP speaks both of them too.

The implementation in this directory was written for Yakumo and tested against running servers; [`docs/SOURCE_PROVENANCE.md`](../../../../docs/SOURCE_PROVENANCE.md#ad-hoc-networking) records what it is based on. Local network discovery (`discovery.cpp`, UDP 27314) is Yakumo's own protocol, described in `discovery.hpp`; other projects are welcome to implement it too.

## Connection to the rest of Yakumo

`session.hpp` and `host.cpp` connect this directory to Yakumo's HLE layer. Three of the functions `session.hpp` declares, `adhoc_apply_settings`, `adhoc_player_name` and `adhoc_session_active`, are defined in `profiles/mhp3rd/host/hle/hle_adhoc.cpp`, which is under the MIT License; `host.cpp` defines the rest.

## Contributing

New ad hoc networking code (protocols, client, server, discovery) goes in this directory, under the same license and exception. Code from here must not be moved into files under the MIT License; MIT-licensed code may be used here.
