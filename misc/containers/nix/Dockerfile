FROM nixos/nix

RUN nix-channel --update && echo "experimental-features = nix-command flakes" > /etc/nix/nix.conf

# The dev shell lists its own packages, so warming it up needs the flake and
# nothing else. Copying the sources here would rebuild this layer on every edit.
COPY ./flake.nix ./flake.lock /doriax-cache-warmup/
COPY ./misc/nix /doriax-cache-warmup/misc/nix

WORKDIR /doriax-cache-warmup/

# Add /opt/workdir as a safe directory in case the entire repository is mounted.
# See the `compose.yaml` file for more information.
RUN git config --global --add safe.directory /opt/workdir
RUN nix develop -c true

WORKDIR /opt/workdir
