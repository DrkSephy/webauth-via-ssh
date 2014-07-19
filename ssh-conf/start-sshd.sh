#!/bin/bash
# keytypes=( dsa ecdsa ed25519 rsa )
# for old ssh 5.1, we only have two:
keytypes=( dsa rsa )
for t in "${keytypes[@]}"; do
	[[ ! -f ./key-$t ]] && ssh-keygen -f key-$t -t $t -q -N ""
done

# NOTE: we unfortunately have to use -q for old openssh 5.1, as it may
# print some debugging information to stdout, compromising the token!!
/home/wes/repos/others/openssh-for-git/sshd -f sshd_config -e -q -D
# /home/wes/repos/others/openssh-for-git/sshd -f sshd_config -e -D
# /usr/bin/sshd -f sshd_config -e -D
# /usr/bin/sshd -f sshd_config -e -d