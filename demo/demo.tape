# VHS demo tape for sash
# https://github.com/charmbracelet/vhs
#
# Run from project root:
#   nix run nixpkgs#vhs -- demo/demo.tape

Output demo.gif

Set Shell "bash"
Set FontSize 14
Set Width 900
Set Height 500
Set Padding 10
Set TypingSpeed 75ms

Type "./build/sash -n 8 -l -w /tmp/build.log ./demo/demo-build.sh"
Sleep 1s
Enter

Sleep 8s

Type "wc -l /tmp/build.log"
Sleep 500ms
Enter
Sleep 2s
