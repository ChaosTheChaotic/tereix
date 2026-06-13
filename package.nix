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
  #nativeCheckInputs = [ cmocka ];
  doCheck = false;
  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    "-DDO_INSTALL=ON"
    "-DUSE_CCACHE=OFF"
    "-DPKG_CONFIG=ON"
		"-DBUILD_TESTING=OFF"
  ];
	meta = {
		description = "The compiler for the Tereix language";
		homepage = "https://github.com/ChaosTheChaotic/tereix";
		license = lib.licenses.gpl3Plus;
		mainProgram = "tereix";
	};
}
