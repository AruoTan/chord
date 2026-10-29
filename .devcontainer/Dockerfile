# SPDX-License-Identifier: CC0-1.0
# SPDX-FileCopyrightText: None

FROM aruowis-dev-base:latest

USER root
COPY install-dependencies.sh /tmp/kdeconnect-install-dependencies.sh
RUN bash /tmp/kdeconnect-install-dependencies.sh \
    && rm /tmp/kdeconnect-install-dependencies.sh
USER vscode
