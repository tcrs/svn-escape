#!/bin/sh

repo=$1
revplus=$2

if [ -z "$repo" ];then
	echo "Usage: sync.sh <svn-repo-name> [numrevisions]"
	exit
fi

# Get the git ref and repo url
ref=$(git config "svn-escape.$repo.ref")
url=$(git config "svn-escape.$repo.url")

# Looks like this repo is not set up, get initial set up info
if [ -z "$ref" ];then
	ref="refs/remotes/svn/$repo"
	echo -n "Enter ref for $repo [$ref]: "
	read uref
	[ ! -z "$uref" ] && ref="$uref"

	git config "svn-escape.$repo.ref" "$ref"

	while [ -z "$url" ];do
		echo -n "Enter repo url for $repo: "
		read url
		git config "svn-escape.$repo.url" "$url"
	done

	echo -n "Enter username for repo $repo: "
	read username
	if [ ! -z "$username" ]; then
		git config "svn-escape.$repo.username" "$username"

		echo -n "Enter password for repo $repo: "
		read passwd
		[ ! -z "$passwd" ] && git config "svn-escape.$repo.password" "$passwd"
	fi
fi

# Get the svn revision and git sha of the last revision synced, or prompt
# the user for the starting revision if this is the first run.
if git log -1 "$ref" > /dev/null 2>&1; then
	sha=$(git log -1 "$ref" | sed -nre 's/^commit ([^ ]+)$/\1/p')
	src=$(git log -1 "$ref" | sed -nre 's/^[ \t]*svn-source: ([^@]+@[0-9]+)$/\1/p')

	srepo=$(echo $src | cut -d@ -f1)
	if [ ! "$repo" = "$srepo" ]; then
		echo "mismatched repo name in config and ref"
		exit
	fi

	rev=$(echo $src | cut -d@ -f2)
	rev=$((rev + 1))
else
	echo -n "Enter parent revision to graft onto (full SHA) [optional]: "
	read sha

	echo -n "Enter revision to start from [0]: "
	read rev
	[ -z "$rev" ] && rev=0
fi

[ ! -z "$revplus" ] && erev=$((rev + revplus))

url=$(git config "svn-escape.$repo.url")
username=$(git config "svn-escape.$repo.username")
passwd=$(git config "svn-escape.$repo.password")

# Do the import
(
	echo "=repo-url $url"
	echo "=repo-name $repo"
	[ ! -z "$username" ] && echo "=username $username"
	[ ! -z "$passwd" ] && echo "=password $passwd"
	echo "=start-rev $rev"
	[ ! -z "$erev" ] && echo "=end-rev $erev"
	echo "=git-ref $ref"
	[ ! -z "$sha" ] && echo "=parent-sha $sha"

	git config --get-all "svn-escape.$repo.ignore" | while read line;do
		echo "=ignore-path $line"
	done
) | svnescape | git fast-import

