#!/bin/sh
# Download+decrypt real game data from release 'gamedata' into data/. Needs GMKEY env.
set -e
[ -n "$GMKEY" ] || { echo "no GMKEY"; exit 1; }
R=${GITHUB_REPOSITORY}
mkdir -p data gd && cd gd
gh release download gamedata -R $R -p '*.bin' || { echo "no gamedata"; exit 1; }
printf %s "$GMKEY" > k
dec() { openssl enc -d -aes-256-cbc -pbkdf2 -pass file:k; }
for l in loose.bin se_loose.bin; do dec < $l | tar xf - -C ../data; rm $l; done
for f in *.bin; do
  n=$(echo "${f%.bin}" | sed 's#_#/#')   # garrysmod_x.vpk -> garrysmod/x.vpk
  dec < "$f" > "../data/$n"; rm "$f"
done
cd .. && rm -rf gd
ln -sfn $PWD/apk/rt data/garrysmod/runtime
du -sh data
