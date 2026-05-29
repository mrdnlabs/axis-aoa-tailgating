ARG ARCH=armv7hf
ARG VERSION=12.9.0
ARG UBUNTU_VERSION=24.04

FROM axisecp/acap-native-sdk:${VERSION}-${ARCH}-ubuntu${UBUNTU_VERSION}

COPY ./app /opt/app/
WORKDIR /opt/app

# Fix permissions from Windows/WSL filesystem (777 → standard Unix)
# Required: device silently skips reverse-proxy rule creation on 777 perms.
RUN find . -type f -exec chmod 644 {} + && \
    find . -type d -exec chmod 755 {} +

RUN . /opt/axis/acapsdk/environment-setup* && acap-build ./
