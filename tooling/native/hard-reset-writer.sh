D=/Users/piano/Desktop
BT=$(sysctl -n kern.boottime | cut -d= -f2 | cut -d, -f1 | tr -d " ")
END=$((BT + 895))
i=0
echo "cycle2 writer start elapsed=$(( $(date +%s) - BT )) deadline=$END"
while [ "$(date +%s)" -lt "$END" ]; do
  i=$((i+1))
  echo "hot $i at $(date +%s)" >> $D/ul12-dirty2.log
  [ $((i % 20)) -eq 0 ] && sync
done
echo "writer finished i=$i"