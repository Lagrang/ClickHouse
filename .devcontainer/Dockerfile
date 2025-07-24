FROM ubuntu:24.04

ARG USERNAME=dev
ARG USER_UID=1001
ARG USER_GID=1001
ARG CLANG_VERSION=19

ENV DEBIAN_FRONTEND=noninteractive
ENV HOME="/home/${USERNAME}"
ENV CMAKE_CXX_COMPILER_LAUNCHER="ccache"
ENV CMAKE_C_COMPILER_LAUNCHER="ccache"
ENV PATH="$PATH:/home/${USERNAME}/.local/bin"

# Disable explicitly, to avoid failing on "Number of CPUs is not deterministic"/"Number of CPUs detected is not deterministic. Per-CPU arena disabled." from jemalloc.
ENV MALLOC_CONF="abort_conf:false,abort:false"

RUN apt update -y && \
    apt install -y ca-certificates software-properties-common git-lfs tzdata wget curl git gnupg vim direnv

# install Cmake from APT repo: https://apt.kitware.com/
RUN wget -O - https://apt.kitware.com/keys/kitware-archive-latest.asc 2>/dev/null | gpg --dearmor - | tee /etc/apt/trusted.gpg.d/kitware.gpg >/dev/null
RUN apt-add-repository "deb https://apt.kitware.com/ubuntu/ $(lsb_release -cs) main"
RUN apt update -y
RUN apt install -y kitware-archive-keyring cmake

RUN wget -O - https://apt.llvm.org/llvm-snapshot.gpg.key | apt-key add -
RUN add-apt-repository "deb http://apt.llvm.org/$(lsb_release -c -s)/ llvm-toolchain-$(lsb_release -c -s)-${CLANG_VERSION} main" && apt update -y

RUN apt update -y && apt install -y \
    python3 pip \
    libssl-dev openssl \
    ninja-build ccache \
    build-essential \
    autotools-dev autoconf \
    nasm \
    yasm \
    lld-${CLANG_VERSION} \
    lldb-${CLANG_VERSION} \
    llvm-${CLANG_VERSION} \
    llvm-${CLANG_VERSION}-dev \
    llvm-${CLANG_VERSION}-runtime \
    llvm-${CLANG_VERSION}-tools \
    clang-${CLANG_VERSION} \
    clangd-${CLANG_VERSION} \
    clang-tidy-${CLANG_VERSION} \
    clang-format-${CLANG_VERSION} \
    libc++-${CLANG_VERSION}-dev \
    libc++abi-${CLANG_VERSION}-dev

RUN update-alternatives --install /usr/bin/ld.lld ld.lld /usr/bin/ld.lld-${CLANG_VERSION} 0
RUN update-alternatives --install /usr/bin/clangd clangd /usr/bin/clangd-${CLANG_VERSION} 0
RUN update-alternatives --install /usr/bin/c++ c++ /usr/bin/clang++-${CLANG_VERSION} 0
RUN update-alternatives --set c++ /usr/bin/clang++-${CLANG_VERSION}
RUN update-alternatives --install /usr/bin/cc cc /usr/bin/clang-${CLANG_VERSION} 0
RUN update-alternatives --set cc /usr/bin/clang-${CLANG_VERSION}

# Add sudo support for the dev user to have full rights in container
RUN apt install -y sudo
# Create the dev user to be used in devcontainer.
# This user will be mapped to the local user on developer's machine.
# These fields can be set in devcontainer.json: "containerUser": "dev", "remoteUser": "dev", "updateRemoteUserUID": true
# to map your local user to the 'dev' user in container.
RUN userdel -r -f ubuntu && groupadd --gid $USER_GID $USERNAME \
    && useradd --uid $USER_UID --gid $USER_GID -m $USERNAME -s /bin/bash \
    && echo $USERNAME ALL=\(root\) NOPASSWD:ALL > /etc/sudoers.d/$USERNAME \
    && chmod 0440 /etc/sudoers.d/$USERNAME

RUN chsh -s /bin/bash root && chsh -s /bin/bash $USERNAME

USER $USERNAME
WORKDIR /home/${USERNAME}

RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | bash -s - -y
RUN . /home/${USERNAME}/.profile && mkdir -p ~/.zfunc ~/.local/share/bash-completion/completions \
    && rustup completions zsh > ~/.zfunc/_rustup \
    && rustup completions bash > ~/.local/share/bash-completion/completions/rustup \
    && rustup toolchain install stable \
    && rustup component add cargo clippy llvm-tools rust-analyzer rust-docs rustfmt \
    && rustup toolchain install nightly-2025-07-07
RUN mkdir /home/${USERNAME}/.ccache
# Required to run of test suites
# RUN pip install jinja2-cli clickhouse_driver scipy

CMD ["/bin/bash"]
