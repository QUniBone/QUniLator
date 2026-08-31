#! /bin/bash
#
# Personalizes a "QUniLator" software tree to "UniBone" or "QBone" hardware.
# Called by github-sync.sh after file update, and by qunilator-devkit after it
# has cloned the tree onto a board.
# Needs hardware-specific settings in "qunibone-platform.env".
#
# Everything works on the tree this script sits in, not on $HOME: it is run
# through sudo as often as not, and $HOME then names whichever account sudo
# decided on rather than the checkout being personalized.
#
# Afterwards "git status" in the tree is not clean, and that is what an
# installed board looks like: 5_applications_u|_q are merged into
# 5_applications and removed, the *.sh are executable, and the shortcuts sit in
# the tree's root directory.
#
#set -v
#set -x
#set -u
# requests a <enter> after each line
#trap read debug

cd "$(dirname "$0")" || exit 1
TREE=$PWD

# Are we running on UniBone or QBone hardware ?
PLATFORMENV="qunibone-platform.env"
if [ ! -f $PLATFORMENV ]; then
  # on old UniBone installation, use "example" file as UniBone config
  cp qunibone-platform.env.example $PLATFORMENV
fi
if [ ! -f $PLATFORMENV ]; then
  echo "Error: Platform settings in file $PLATFORMENV not found!"
  exit 1
fi


. $PLATFORMENV

# fix legacy qunibone-platform.env: QUNILATOR_PLATFORM was MAKE_QUNIBUS
# QUNILATOR_PLATFORM_SUFFIX was PLATFORM_SUFFIX
if [ -z "$QUNILATOR_PLATFORM_SUFFIX" ] ; then
        QUNILATOR_PLATFORM_SUFFIX=$PLATFORM_SUFFIX
fi
if [ -z "$QUNILATOR_PLATFORM" ] ; then
        QUNILATOR_PLATFORM=$MAKE_QUNIBUS
fi


if [ -z "$QUNILATOR_PLATFORM" ]; then
  echo "Error: variable QUNILATOR_PLATFORM not set or empty!"
  exit 1
fi
if [ -z "$QUNILATOR_PLATFORM_SUFFIX" ]; then
  echo "Error: variable QUNILATOR_PLATFORM_SUFFIX not set or empty!"
  exit 1
fi


#################################################################
# make_link()
# create symbolic link "linkpath" for "filepath"
# linkpath & filepath result of "link4*" functions
function make_link {

  if [ "$filepath" == "$linkpath" ]; then
    echo "Error: no _u/_q variant for $linkpath found!"
    exit 1
  fi

  # is there a file or directory containing  PLATFORM_SUFFIX?
  if [ ! -f "$filepath" ] && [ ! -d "$filepath" ] ; then
    echo "Error: $filepath does not exist!"
    exit 1
  fi

  # any match: create the link
  ln -f -s $filepath $linkpath
  echo "Created link $linkpath for $filepath"
}

#################################################################
# link4sh()
# create link for _u/_q shell file
function link4sh() {
  # parameter: name of simpler name to _u.sh or _q.sh file
  linkpath="$1"

  # see https://tldp.org/LDP/abs/html/string-manipulation.html
  # try to match "dir/filename_u.sh"

  substr=.sh
  replace=${PLATFORM_SUFFIX}.sh
  # find at end of file
  filepath=${linkpath/%$substr/$replace}

  make_link
  return $?
}

#################################################################
# link4dir()
# create link for _u/_q directory, which is created if missing
# Example:
# ln -s  10.03_app_demo/4_deploy_q 10.03_app_demo/4_deploy
function link4dir() {
  # try suffix at end of file/directoy: 4_deploy_u -> 4_deploy
  linkpath="$1"

  substr=
  replace=${QUNILATOR_PLATFORM_SUFFIX}
  filepath=${linkpath/%$substr/$replace}

  mkdir -p $filepath

  # create symbolic link "linkpath" for "filepath"
  make_link

  return $?
}

#################################################################
# main()

# link4sh ./compile.sh


# The repository carries the examples of both buses.
# 10.03_app_demo/5_applications are sorted into
#       "...5_applications" (identical for UNIBUS and QBUS machines: the
#        scripts, listings and images that are the same on both)
# and   "...5_applications_q" (runs only on QBUS)
# and   "...5_applications_u" (runs only on UNIBUS)
# A bus-specific script may name an image of the common tree and the other way
# round; the two only meet after the copy below, which is what makes the
# installed 5_applications complete.

# Final Installation: only 5_applications with all fitting apps
# if UniBone: copy 5_applications_u/* to 5_applications,
# if QBone: copy 5_applications_q/* to 5_applications,

appdir=$TREE/10.03_app_demo/5_applications
platform_appdir=$appdir$QUNILATOR_PLATFORM_SUFFIX
mkdir -p "$appdir"
if [ -d "$platform_appdir" ] ; then
  echo "Copying 5_applications$QUNILATOR_PLATFORM_SUFFIX to 5_applications"
  # (recursive move faster, but complicate directory merge)
  # "/." instead of "/*": copies dot files too, and does not fail on an empty
  # directory
  cp -f -a "$platform_appdir/." "$appdir"
else
  echo "No 5_applications$QUNILATOR_PLATFORM_SUFFIX, only the platform independent 5_applications"
fi

# In any case: remove 5_applications_u and 5_applications_q
rm -f -R  "$TREE/10.03_app_demo/5_applications_u"
rm -f -R  "$TREE/10.03_app_demo/5_applications_q"

# An example is run by the emulator's menu program, which it names on its "#!"
# line, and that program carries the board's name: /usr/bin/qbone-cli on a QBUS
# board, /usr/bin/unibone-cli on a UNIBUS one. The bus-specific examples name
# theirs already; the ones that serve both buses are written in the QBone
# spelling, as the packaging is, and are rebranded here.
SUFFIX=$QUNILATOR_PLATFORM_SUFFIX
if [ -f "$TREE/packaging/board.sh" ] ; then
  . "$TREE/packaging/board.sh"
else
  # a tree predating packaging/board.sh: the same mapping, spelled out
  if [ "$SUFFIX" = _u ] ; then NAME=unibone ; else NAME=qbone ; fi
fi
echo "Naming the examples' interpreter /usr/bin/$NAME-cli"
find "$appdir" -type f -name \*.sh -exec \
  sed -i "1s|^#!/usr/bin/qbone-cli|#!/usr/bin/$NAME-cli|" '{}' \;

# Shortcuts to the example scripts, in the root of the tree - which on a board
# is /root, the directory an operator lands in. A shortcut never overwrites a
# file which is not one: the scripts of the tree itself live there too.
while read -r script ; do
  linkname=$TREE/$(basename "$script")
  if [ -e "$linkname" ] && [ ! -L "$linkname" ] ; then
    echo "Not linking $script: $linkname exists and is not a link"
    continue
  fi
  ln -sf "$script" "$linkname"
done < <(find "$appdir" -name \*.sh)

link4dir "$TREE/10.03_app_demo/4_deploy"

# Assure all shell scripts are executable
find "$TREE" -name '*.sh' -exec chmod +x '{}' \;

# remove broken links, if any remaining: an example the merge above did not
# bring in leaves its shortcut behind
find "$TREE" -xtype l -delete



# Assure all shell scripts are executable
find . -name '*.sh' -exec chmod +x '{}' \;

# remove broken links, if any remaining
find . -xtype l -delete

