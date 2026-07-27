# dmdhcp

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![CI](https://github.com/choco-technologies/dmdhcp/actions/workflows/ci.yml/badge.svg)](https://github.com/choco-technologies/dmdhcp/actions/workflows/ci.yml)

dmdhcp DMOD library module.

## Description

TODO: describe what this module does.

## Building

### Using CMake

```bash
mkdir -p build
cd build
cmake ..
cmake --build .
```

Pass `-DDMOD_DIR=/path/to/local/dmod` to build against a local dmod checkout
instead of fetching `develop` from GitHub.

### Using Make

```bash
make DMOD_MODE=DMOD_MODULE DMOD_DIR=/path/to/dmod
```

## Testing

Tests are built automatically alongside the module (see `tests/`). Once built,
run them with `ctest`:

```bash
cd build
ctest --output-on-failure
```

`ctest` installs the test module's dependencies with `dmf-get` and then runs
it through `dmod_loader`. To run it manually instead:

```bash
export DMOD_DMF_DIR=$(pwd)/build/dmf
dmf-get install -d ${DMOD_DMF_DIR}/test_dmdhcp-local.dmd -y
dmod_loader build/dmf/test_dmdhcp.dmf
```

## Usage

<TBD>

This library module provides functions that can be used by other modules:

```c
#include "dmdhcp.h"
```

## API

| Function | Description |
|----------|-------------|
| `dmdhcp_create()` | Create a new `dmdhcp_t` instance. |
| `dmdhcp_destroy()` | Destroy an instance created by `_create()`. |
| `dmdhcp_is_valid()` | Check whether a handle is a valid instance. |

See [include/dmdhcp.h](include/dmdhcp.h) for the full
declarations and [docs/api-reference.md](docs/api-reference.md) for the
complete reference.

## Documentation

See the `docs/` directory:

- **[api-reference.md](docs/api-reference.md)** - Complete API documentation

View documentation using `dmf-man dmdhcp`.
## Project Structure

```
dmdhcp/
├── docs/              # Documentation (markdown format)
├── include/           # Public headers
│   └── dmdhcp.h
├── src/
│   └── dmdhcp.c
├── tests/
│   ├── CMakeLists.txt
│   └── dmdhcp_test.c
├── CMakeLists.txt
├── Makefile
├── dmdhcp.dmr
└── manifest.dmm
```

## Author

Patryk Kubiak

## License

MIT
