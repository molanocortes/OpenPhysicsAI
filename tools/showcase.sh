#!/bin/sh
# Renders a gallery of off-screen screenshots of the preset experiments.
#   tools/showcase.sh [steps]        (run from anywhere; images go to screenshots/)
set -e
cd "$(dirname "$0")/.."
STEPS=${1:-3000}
mkdir -p screenshots
for scene in glider cylinder car sphere; do
    echo "rendering $scene ($STEPS steps)..."
    ./navier --headless --size 1600x1000 --scene "$scene" --steps "$STEPS" --shot "screenshots/$scene.png" > /dev/null 2>&1
done
echo "done: screenshots/*.png"
