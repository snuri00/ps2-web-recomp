# Sourced by setup.sh and port.sh: sets cxx_flags to the CMake arguments for a
# C++ compiler that has <format>. $CXX wins when it works; otherwise the newest
# clang++ (with its own libc++ if the system libstdc++ predates GCC 13), then g++.
cxx_flags=()
cxx_probe=$(mktemp -d)
printf '#include <format>\nint main() { return std::format("{}", 1).size() != 1; }\n' > "$cxx_probe/t.cpp"
cxx_candidates=()
[ -n "${CXX:-}" ] && cxx_candidates+=("$CXX")
cxx_candidates+=($(compgen -c clang++- | grep -E '^clang\+\+-[0-9]+$' | sort -t- -k2 -rn | uniq || true) clang++ g++)
for cxx in "${cxx_candidates[@]}"; do
  command -v "$cxx" > /dev/null || continue
  for stdlib in "" -stdlib=libc++; do
    if "$cxx" -std=c++20 $stdlib "$cxx_probe/t.cpp" -o "$cxx_probe/t" 2> /dev/null; then
      cxx_flags=(-DCMAKE_CXX_COMPILER="$(command -v "$cxx")")
      [ -z "$stdlib" ] || cxx_flags+=(-DCMAKE_CXX_FLAGS="$stdlib")
      break 2
    fi
  done
done
rm -rf "$cxx_probe"
if [ ${#cxx_flags[@]} -eq 0 ]; then
  echo "no C++ compiler here has <format>: install clang 17+ with libc++ (e.g. clang-19 libc++-19-dev) or GCC 13+, or point CXX at one" >&2
  exit 1
fi
