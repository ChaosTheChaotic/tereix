{ stdenv
, lib
, cmake
, ninja
, pkg-config
, cmocka
}:

stdenv.mkDerivation {
  pname = "tereix";
  version = "0.1.0";
  src = ./.;

  nativeBuildInputs = [
    cmake
    ninja
    pkg-config
  ];

  buildInputs = [
    # Dependencies managed by minit conf --add
  ];
  nativeCheckInputs = [ cmocka ];
  doCheck = true;
  cmakeFlags = [
    "-DDO_INSTALL=ON"
    "-DUSE_CCACHE=OFF"
    "-DPKG_CONFIG=ON"
  ];
}
