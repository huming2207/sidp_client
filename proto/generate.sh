#!/bin/sh
# Regenerate the nanopb sources after editing a .proto or .options file.
#
# NANOPB_DIR must point at a nanopb 0.4.9.2 source tree, the same version as
# the runtime vendored in ../nanopb. Its generator needs Python with the
# protobuf and grpcio-tools packages (see nanopb's requirements.txt).
#
# sidp-agent keeps a copy of the .proto and .options files; update it as well.
set -e
: "${NANOPB_DIR:?set NANOPB_DIR to a nanopb 0.4.9.2 source tree}"
cd "$(dirname "$0")"
for proto in job manage; do
    python3 "$NANOPB_DIR/generator/nanopb_generator.py" -f "$proto.options" "$proto.proto"
done
