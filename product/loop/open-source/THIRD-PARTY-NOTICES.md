# External dependency notices

The source tree retains each included MIT license notice.
External libraries, archives, models, and data retain their own terms.
This file identifies sources; it does not replace their complete notices.

| Dependency | Declared terms | Source |
|---|---|---|
| PGlite | Apache-2.0 | https://github.com/electric-sql/pglite |
| TypeScript | Apache-2.0 | https://github.com/microsoft/TypeScript |
| Zod | MIT | https://github.com/colinhacks/zod |
| Bun and Bun types | MIT and upstream notices | https://github.com/oven-sh/bun |
| Node and Undici type packages | MIT | https://github.com/DefinitelyTyped/DefinitelyTyped and https://github.com/nodejs/undici |
| OpenSSL 3 | Apache-2.0 | https://openssl-library.org/source/license/index.html |
| SQLite library | Public domain | https://www.sqlite.org/copyright.html |
| LSQUIC | MIT | https://github.com/litespeedtech/lsquic/blob/19547405c24f60c4537478d38f4214e990be1f95/LICENSE |
| AWS-LC | Multiple upstream licenses | https://github.com/aws/aws-lc/blob/991e67ff4cf04df4dd89e407f8b920c6936cb56a/LICENSE |
| IBM Plex fonts | SIL Open Font License 1.1 | https://github.com/IBM/plex/blob/763c36ef9117782905ae010056dfbe8fd2653a25/LICENSE.txt |

`product/loop/vendor.lock.json` records exact archive locations, sizes, hashes, and license declarations.
The fetcher verifies archive bytes before installation.
It downloads dependencies to the operator's working copy and does not republish them.
Alpine packages include GPL and LGPL components with corresponding-source obligations for some forms of distribution.
The base filesystem and binary images need their own complete component inventory and notices before redistribution.

The public source candidate includes no model weights or voice fixtures.
The five included fonts retain their complete notice in `product/loop/static/fonts/OFL.txt`.
Their exact bytes match the pinned public sources in `font-rights.json`.
