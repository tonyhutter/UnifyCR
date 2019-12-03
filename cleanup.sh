
# Remove shared directory from starting unifycrd (fill in with your shared dir)
rm -fr ~/shared/* ~/data/* ~/meta/*

# Remove stuff in /tmp
rm -fr /tmp/kvstore /tmp/na_sm_`whoami` /tmp/unifyfs-runstate.conf* /tmp/unifyfs_db* /tmp/unifyfsd.log* /tmpunifyfsd.margo-shm
echo "cleaned up"
