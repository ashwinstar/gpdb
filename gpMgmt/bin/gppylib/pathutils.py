#!/usr/bin/env python
# Line too long - pylint: disable=C0301
# Invalid name  - pylint: disable=C0103

"""
  pathutils.py
  Routines to replace paths for tools like gpmovemirrors
  Copyright (c) EMC/Greenplum Inc 2011. All Rights Reserved. 
"""

import os
os_path_split = os.path.split
os_path_join  = os.path.join


def pathsplit(p, rest=None):
    # More than one statement on a single line - pylint: disable=C0321
    """
    @param p: the input path
    @returns: the path split into a component list:  /a/b/c -> ['/', 'a', 'b', 'c']
    """
    (h, t) = os_path_split(p)
    if rest is None: rest = []
    if len(h) < 1:   return [t] + rest
    if len(t) < 1:   return [h] + rest
    return pathsplit(h, [t]+rest)


def filespaces(array):
    """
    Generates a sequence of filespaces for the array.
    @param array: the gparray
    @returns: array filespaces
    """
    for db in array.getDbList(False):
        for _, path in db.getSegmentFilespaces().items():
            yield path


def mirrorfilespaces(array):
    """
    Generates a sequence of filespaces for the mirrors in the array.
    @param array: the gparray
    @returns: array filespaces
    """
    for db in array.getDbList(False):
        if db.isSegmentMirror():
            for _, path in db.getSegmentFilespaces().items():
                yield path


def pathtree(paths):
    """
    Build a tree out of a sequence of paths, e.g.

    /a/b/c
    /a/b/c2 -> { '/': {'a': {'b': {'c': {}, 'c2':{}}}}}

    @param paths: the path sequence
    @returns: a tree of path components
    """
    tree  = {}
    for path in paths:
        subtree = tree
        for component in pathsplit(path):
            n = subtree.get(component)
            if n is None:
                subtree[component] = n = {}
            subtree = n
    return tree


def prefixtree(paths):
    """
    Build a tree out of a sequence of paths, ignoring the last component.  e.g.

    /a/b/c/gp1
    /a/b/c2/gp2 -> { '/': {'a': {'b': {'c': {}, 'c2':{}}}}}

    @param paths: the path sequence
    @returns: a tree of path components
    """
    tree  = {}
    for path in paths:
        subtree = tree
        for component in pathsplit(path)[0:-1]:
            n = subtree.get(component)
            if n is None:
                subtree[component] = n = {}
            subtree = n
    return tree


def pathwalk(tree, _path=''):
    """
    Generate a series of paths from a tree built with filespaces() or  mirrorfilespaces().

    @param tree: the tree to walk
    @param _path: temporary argument for recursion holding path built up so far
    @returns: a series of paths
    """
    for k in tree.keys():
        subtree = tree[k]
        subpath = os_path_join(_path, k)

        # recursively generate subtree paths
        if len(subtree) > 0:
            for more in pathwalk(subtree, subpath):
                yield more
        else:
            yield subpath


def pathreplace(tree, fromto, _path='', _new='', _replaced=False):
    """
    Generate a series of paths from a tree built with filespaces() or mirrorfilespaces(),
    performing replacements as specified by a replacement map.  

    @param tree:      the tree to walk
    @param fromto:    mapping rules
    @param _path:     temporary argument for recursion holding path built up so far
    @param _new:      temporary argument for recursion holding replacement path built up so far
    @param _replaced: temporary argument for recursion tracking whether or not we've done a replacement
    @returns: a series of (oldpath, newpath, replaced) triples
    """

    # attempt replacement of current path in '_new'
    #print fromto, _path, _new, _replaced

    _to = fromto.get(_new)
    if _to is not None:
        _replaced = True
        _new = _to

    for k in tree.keys():
        subtree = tree[k]
        subpath = os_path_join(_path, k)
        subnew  = os_path_join(_new, k) if _replaced else subpath

        # recursively generate subtree paths
        if len(subtree) > 0:
            for nextpath, nextnew, nextrep in pathreplace(subtree, fromto, subpath, subnew, _replaced):
                yield nextpath, nextnew, nextrep
        else:
            yield subpath, subnew, _replaced



if __name__ == '__main__':
    def main():
        "simple test"
        from catalog import GpArray
        array  = GpArray()
        tree   = prefixtree( mirrorfilespaces( array ))
        print '-- mirrorfilespaces'
        for path in pathwalk(tree, ''):
            print path
        fromto = {
             '/data':      '/u/data'
           , '/otherdata': '/u/data'
        }
        print '-- replacing', fromto
        for old, new, replaced in pathreplace(tree, fromto):
            print old, '>', new, replaced

    main()
