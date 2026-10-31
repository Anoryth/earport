{
  description = "EarPort — AirPods integration for GNOME";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      inherit (nixpkgs) lib;
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = lib.genAttrs systems;

      src = lib.cleanSource ./.;

      version = builtins.head
        (builtins.match ".*version: '([0-9.]+)'.*" (builtins.readFile ./daemon/meson.build));
    in
    {
      packages = forAllSystems (system:
        let
          pkgs = nixpkgs.legacyPackages.${system};
          inherit (pkgs) stdenv meson ninja pkg-config glib bluez gettext;

          earport-daemon = stdenv.mkDerivation {
            pname = "earport-daemon";
            inherit version src;
            nativeBuildInputs = [ meson ninja pkg-config glib ]; # glib: gdbus-codegen
            buildInputs = [ glib bluez ];                       # bluez: l2cap.h header only
            preConfigure = "cd daemon";                         # meson.build lives in daemon/
            meta = with lib; {
              description = "EarPort AirPods integration daemon";
              homepage = "https://github.com/Anoryth/earport";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
              mainProgram = "earport-daemon";
            };
          };

          earport-extension = stdenv.mkDerivation {
            pname = "earport-extension";
            inherit version src;
            nativeBuildInputs = [ glib gettext ]; # glib-compile-schemas, msgfmt
            dontBuild = true;
            installPhase = ''
              runHook preInstall
              mkdir -p "$out/share/gnome-shell/extensions/earport@anoryth.github.io"
              bash tools/pack-extension.sh --dir \
                "$out/share/gnome-shell/extensions/earport@anoryth.github.io"
              # GNOME cannot compile the schema into the read-only store at
              # runtime, so compile it here.
              glib-compile-schemas \
                "$out/share/gnome-shell/extensions/earport@anoryth.github.io/schemas"
              runHook postInstall
            '';
            meta = with lib; {
              description = "EarPort GNOME Shell extension";
              homepage = "https://github.com/Anoryth/earport";
              license = licenses.gpl3Plus;
              platforms = platforms.linux;
            };
          };
        in
        {
          inherit earport-daemon earport-extension;
          default = earport-daemon;
        });

      homeManagerModules.default = { config, lib, pkgs, ... }:
        let cfg = config.services.earport;
        in
        {
          options.services.earport = {
            enable = lib.mkEnableOption "EarPort AirPods integration";

            package = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.earport-daemon;
              description = "The EarPort daemon package.";
            };

            extensionPackage = lib.mkOption {
              type = lib.types.package;
              default = self.packages.${pkgs.stdenv.hostPlatform.system}.earport-extension;
              description = "The EarPort GNOME Shell extension package.";
            };
          };

          config = lib.mkIf cfg.enable {
            home.packages = [ cfg.package cfg.extensionPackage ];

            systemd.user.services.earport-daemon = {
              Unit = {
                Description = "EarPort AirPods daemon";
                After = [ "bluetooth.target" ];
              };

              Service = {
                Type = "dbus";
                BusName = "io.github.anoryth.EarPort";
                ExecStart = "${cfg.package}/bin/earport-daemon";
                Restart = "on-failure";
                RestartSec = 5;
                NoNewPrivileges = true;
                ProtectSystem = "strict";
                ProtectHome = "read-only";
                PrivateTmp = true;
                ConfigurationDirectory = "earport";
                ConfigurationDirectoryMode = "0700";
                # The daemon restarts AirPods audio by shelling out to a
                # script (daemon/src/audio_route.c) that calls pactl, awk and
                # sleep. Without these on the service's PATH, switching to an
                # Apple device fails silently. sh comes with bash, pactl with
                # pulseaudio, awk with gawk and sleep with coreutils.
                Environment = [
                  "PATH=${lib.makeBinPath [ pkgs.pulseaudio pkgs.gawk pkgs.coreutils pkgs.bash ]}"
                ];
              };

              Install.WantedBy = [ "default.target" ];
            };
          };
        };

      homeManagerModules.earport = self.homeManagerModules.default;

      devShells = forAllSystems (system:
        let pkgs = nixpkgs.legacyPackages.${system};
        in {
          default = pkgs.mkShell {
            packages = with pkgs; [ meson ninja pkg-config glib bluez gettext zip git dbus ];
          };
        });
    };
}
