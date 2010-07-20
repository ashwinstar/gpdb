/*
 * Copyright (c) 2009 - Greenplum 
 */
package org.postgresql.pljava.sqlj;

import java.io.IOException;
import java.io.ByteArrayOutputStream;
import java.net.URL;
import java.net.MalformedURLException;
import java.util.Collections;
import java.util.Enumeration;
import java.util.HashMap;
import java.util.Map;
import java.util.NoSuchElementException;
import java.util.logging.Level;
import java.util.logging.Logger;
import java.util.jar.Attributes;
import java.util.jar.JarEntry;
import java.util.jar.JarInputStream;

/**
 * @author Caleb Welton
 */
public class JarLoader extends ClassLoader
{
	private final URL m_url;
	private final Map m_entries;
	private Map       m_images;

	/**
	 * Creates a loader to load a specified JAR file.
	 *
	 * We place no restrictions on what the <code>URL</code> points to here.
	 * instead any access restrictions (from trusted java) are enforced by 
	 * the <code>Security Manager</code> defined in <code>Backend.java</code>.
	 *
	 * @param url : The URL to load
	 * @return JarLoader : Classloader for a specified JAR
	 * @throws IOException : when the Jar cannot be read
	 *
	 */
	JarLoader(URL url) throws IOException
	{
		super(JarLoader.class.getClassLoader());

		m_url = url;
		m_entries = new HashMap();
		m_images  = new HashMap();

		// Just having a JAR in your CLASSPATH will cause us to load it,
		// which means you need security access to it.
		JarInputStream jis = new JarInputStream(url.openStream());
		ByteArrayOutputStream img = new ByteArrayOutputStream();
		byte[] buf = new byte[1024];
		JarEntry je;
		for (je = jis.getNextJarEntry();
			 je != null;
			 je = jis.getNextJarEntry())
		{
			if (je.isDirectory())
				continue;
			String entryName = je.getName();
			Attributes attr = je.getAttributes();
			int nBytes;
			
			img.reset();
			while ((nBytes = jis.read(buf)) > 0)
				img.write(buf, 0, nBytes);
			jis.closeEntry();
				
			m_entries.put(entryName, img.toByteArray());
		}
	}

	/**
	 * Finds the class with the specified name.  This method is invoked by the 
	 * inherited <code>loadClass</code> method after checking the parent class
	 * loader for the requested class.
	 *
	 * For security reasons it is necessary to disable loading new classes
	 * into the <code>java.</code> or <code>org.postgresql.pljava.</code>
	 * namespaces.  Otherwise it would be possible for a user to create a
	 * class in this namespace.  Allowing new classes in this namespace
	 * would allow access to package-protected variables, manipulation of
	 * which could lead to a subversion of existing security measure.  Any
	 * attempt to load classes from this namespace will result in a 
	 * <code>ClassNotFoundException</code>.
	 *
	 * @param name - the binary name of the class
	 * @return The resulting <code>Class</code> object
	 * @throws ClassNotFoundException - If the class could not be found
	 */
	protected Class findClass(final String name)
		throws ClassNotFoundException
	{
		/* Protect the namespace */
		if (name.startsWith("java.") ||
			name.startsWith("org.postgresql.pljava"))
		{
			throw new ClassNotFoundException(name); 
		}

		String path = name.replace('.', '/').concat(".class");
		byte[] entryImg = (byte[]) m_entries.get(path);
		
		/* If not found, raise an exception */
		if (entryImg == null)
			throw new ClassNotFoundException(name);

		/* otherwise convert the image to a class and return it */
		return this.defineClass(name, entryImg, 0, entryImg.length);
	}

	protected URL findResource(String name)
	{
		/* Lookup the name */
		String path = name.replace('.', '/').concat(".class");
		Object entryImg = m_entries.get(path);
		
		/* Return null when not found */
		if (entryImg == null)
			return null;

		try 
		{
			URL url = new URL("jar",
							  m_url.getHost(),
							  m_url.getPath() + "!" + path);
			return url;
		}
		catch (MalformedURLException e)
		{
			throw new RuntimeException(e);
		}
	}

}
