#!/usr/bin/env python
# Line too long - pylint: disable=C0301
# Invalid name  - pylint: disable=C0103

"""
  pathutils.py
  Routines to replace paths for tools like gpmovemirrors
  Copyright (c) EMC/Greenplum Inc 2011. All Rights Reserved. 
"""

from os.path import join as os_path_join, sep as os_path_sep

def pathsplit(p):
    """
    Split a path into a list of components.  For example:

    >>> pathsplit('')
    []
    >>> pathsplit('abc')
    ['abc']
    >>> pathsplit('abc/def')
    ['abc', 'def']
    >>> pathsplit('abc//def')
    ['abc', 'def']
    >>> pathsplit('abc//def/')
    ['abc', 'def']
    >>> pathsplit('/')
    ['/']
    >>> pathsplit('//')
    ['/']
    >>> pathsplit('/abc')
    ['/', 'abc']
    >>> pathsplit('/abc/def')
    ['/', 'abc', 'def']
    >>> pathsplit('/abc//def//')
    ['/', 'abc', 'def']

    @param p: the input path
    @returns: the path split into a component list:  /a/b/c -> ['/', 'a', 'b', 'c']
    """

    l = p.split(os_path_sep)
    if len(l) > 1:
        if l[0] == '': 
            l[0] = os_path_sep
    return [c for c in l if c]



def pathtree(paths):
    """
    Build a tree out of a sequence of paths, e.g.

    >>> pathtree([])
    {}
    >>> pathtree(['/a/b/c', '/a/b/c2'])
    {'/': {'a': {'b': {'c2': {}, 'c': {}}}}}

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

    >>> prefixtree([])
    {}
    >>> prefixtree(['/a/b/c/gp1', '/a/b/c2/gp2'])
    {'/': {'a': {'b': {'c2': {}, 'c': {}}}}}

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

    >>> [p for p in pathwalk( pathtree(['/a/b/c', '/a/b/c2']) )]
    ['/a/b/c2', '/a/b/c']

    @param tree: the tree to walk
    @param _path: temporary argument for recursion holding path built up so far
    @returns: a series of paths
    """
    if len(tree) < 1:
        yield _path

    else:
        # recursively generate subtree paths
        for k in tree.keys():
            subtree = tree[k]
            subpath = os_path_join(_path, k)

            for more in pathwalk(subtree, subpath):
                yield more


def pathreplace(tree, fromto, _path='', _new='', _replaced=False):
    """
    Generate a series of paths from a tree built with filespaces() or mirrorfilespaces(),
    performing replacements as specified by a replacement map.  

    >>> [(o,n,r) for (o,n,r) in pathreplace( pathtree(['/a/b/c', '/d/e/f']), {'/a':'/x'} )]
    [('/a/b/c', '/x/b/c', True), ('/d/e/f', '/d/e/f', False)]

    @param tree:      the tree to walk
    @param fromto:    mapping rules
    @param _path:     temporary argument for recursion holding path built up so far
    @param _new:      temporary argument for recursion holding replacement path built up so far
    @param _replaced: temporary argument for recursion tracking whether or not we've done a replacement
    @returns: a series of (oldpath, newpath, replaced) triples
    """

    # attempt replacement of current path in '_new'
    _to = fromto.get(_new)
    if _to is not None:
        _replaced = True
        _new = _to

    if len(tree) < 1:
        yield _path, _new, _replaced

    else:
        # recursively generate subtree paths
        for k in tree.keys():
            subtree = tree[k]
            subpath = os_path_join(_path, k)
            subnew  = os_path_join(_new, k) if _replaced else subpath

            for nextpath, nextnew, nextrep in pathreplace(subtree, fromto, subpath, subnew, _replaced):
                yield nextpath, nextnew, nextrep


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


if __name__ == '__main__':
    import doctest
    doctest.testmod()

