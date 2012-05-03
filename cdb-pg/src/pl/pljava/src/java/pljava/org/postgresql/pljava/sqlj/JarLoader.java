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
import java.util.Set;
import java.util.NoSuchElementException;
import java.util.jar.Attributes;
import java.util.jar.Attributes.Name;
import java.util.jar.JarEntry;
import java.util.jar.JarInputStream;
import java.util.jar.Manifest;
import java.util.ArrayList;


/**
 * @author Caleb Welton
 */
public class JarLoader extends ClassLoader
{
	private final URL m_url;
	private final Map<String, byte[]> m_entries;
	private Manifest  m_manifest;
	private final ClassLoader m_parent;

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
	JarLoader(ClassLoader parent, URL url) throws IOException
	{
		super(parent);

		m_parent = parent;
		m_url = url;
		m_entries = new HashMap<String, byte[]>();

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
		m_manifest = jis.getManifest();
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

		/* Look to see if we have already loaded the class */
		Class c = findLoadedClass(name);
		if (c != null)
			return c;

		/* Check for the class within our jar */
		String path = name.replace('.', '/').concat(".class");
		
		byte[] entryImg = (byte[]) m_entries.get(path);
		
		/* If not found, raise an exception */
		if (entryImg == null)
			throw new ClassNotFoundException(name);

		/* create a package for the class */
		definePackage(name);

		/* otherwise convert the image to a class and return it */
		return defineClass(name, entryImg, 0, entryImg.length);
	}

	/**
	 * Construct the URL of a resource with the given name
	 * 
	 * @param name	the resouce name with relative path
	 *
	 * @return The URL of the resource name which is case sensitive
	 *
	 * @throws MalformedURLException if the URL is illegal
	 * 
	 */
	private URL getURL(String name)
		throws MalformedURLException
	{
		return new URL
			(
				"jar",
				m_url.getHost(),
				"file:" + m_url.getPath() + "!/" + name
			);
	}
	
	/**
	 * Find the resource with the given name.
	 * For example, if we have three resources (A.class, dir1/A.class and 
	 * dir2/A.class), then findResource("A.class") will only return 
	 * the URL of first resource. if you want to get the URL of "dir1/A.class", 
	 * then you should invoke findResource("dir1/A.class").
	 * 
	 * @param name	The resouce name which is case sensitive.
	 *
	 * @return A <tt>URL</tt> object for reading the resource, or
	 *         <tt>null</tt> if the resource could not be found
	 *
	 */
	protected URL findResource(String name)
	{
		/* Lookup the name */
		Object entryImg = m_entries.get(name);
		
		/* Return null when not found */
		if (entryImg == null)
		{
			return null;
		}

		try 
		{
			return getURL(name);
		}
		catch (MalformedURLException e)
		{
			throw new RuntimeException(e);
		}
	}

	/**
	 * Returns an enumeration of {@link java.net.URL <tt>URL</tt>} objects 
	 * representing all the resources with the given name.
	 * For example, if we have three resources (A.class, dir1/A.class and 
	 * dir2/A.class), then findResources("A.class") will the URLs of 
	 * these three resources.
	 * 
	 * @param name The resource name which is case sensitive.
	 *
	 * @return An enumeration of {@link java.net.URL <tt>URL</tt>} objects for
	 *         the resources.
	 *
	 * @throws  IOException If I/O errors occur.
	 *
	 */
	protected Enumeration<URL> findResources(String name)
		throws IOException
	{
		ArrayList<URL> res = new ArrayList<URL>();

		/*
		 * Go through all the resources, find the resources 
		 * which matches with the given name 
		 */
		for (String entryName : m_entries.keySet())
		{
			int len = entryName.length() - name.length();
			/*
			 * (0 == len) means they are exactly matched
			 * ('/' == entryName.charAt(len -1)) means they are partially matched, 
			 * such as dir/A.class partially matches with dir/dir/A.class 
			 */
			if (entryName.endsWith(name) && ((0 == len) || ('/' == entryName.charAt(len -1))))
			{
				try
				{
					res.add(getURL(entryName));
				}
				catch(MalformedURLException e)
				{
					throw new IOException(e);
				}
			}
		}

		return Collections.enumeration(res);
	}

	/**
	 * Retrieve all the resources which match with the given name.
	 * This method will search the parent class loader for the
	 * resources; if the parent is <tt>null</tt> the path of the class loader
	 * built-in to the virtual machine is searched. If no resources could be 
	 * found, the enumeration will be empty.
	 *
	 * For example, if we have three resources (A.class, dir1/A.class and 
	 * dir2/A.class), then findResources("A.class") will return the URLs of  
	 * these three resources.
	 *
	 * According to the default behavior of getResources, the method 
	 * findResources in both the parent class loader (Loader) and the current 
	 * class loader (JarLoader) will be invoked. As our Loader will call the method
	 * findResources through all JarLoaders (include current JarLoader), the 
	 * resources in the current JarLoader will be search twice. To avoid invoking
	 * the current JarLoader twice, we override this method.	 
	 * 
	 * @param name	The resouce name which is case sensitive.
	 *
	 * @return The enumeration of URLs matches with the given name
	 *
	 * @throws  IOException If I/O errors occur.
	 *
	 */	
	public Enumeration<URL> getResources(String name) throws IOException
	{
		// The parent will find the resources through all the jar files
		if (m_parent != null)
		{
			return m_parent.getResources(name);
		}

		/*
		 * If parent is null, let the following method to call 
		 * getBootstrapResources. Otherwise, we need to introduce
		 * sun.misc.Resource when implementing getBootstrapResources.
		 * According to the default behavior of the following method,
		 * it will call this.getResources(name) again. However, empty
		 * enumeration will be return. Therefore, the result is correct.
		 * 
		 */
		return super.getResources(name);
	}

	/**
	 * Define the package information associated with a class.
	 */
	protected void definePackage(String className) 
	{
		int    classIndex  = className.lastIndexOf('.');
		String specTitle   = null;
		String specVendor  = null;
		String specVersion = null;
        	String implTitle   = null;
        	String implVendor  = null;
        	String implVersion = null;
        	String sealed      = null;
        	URL    sealBase    = null;

		/* Quick return if the class has no package */
        	if (classIndex == -1)
            		return;

		/* Or if the package has already been defined */
        	String packageName = className.substring(0, classIndex);
		if (getPackage(packageName) != null)
            		return;

		/* Read package information from the manifest, if any */
        	if (m_manifest != null) 
        	{
			String     sectionName       = packageName.replace('.', '/') + "/";
			Attributes sectionAttributes = m_manifest.getAttributes(sectionName);
			Attributes mainAttributes    = m_manifest.getMainAttributes();
			
			if (sectionAttributes != null)
			{
				specTitle   = sectionAttributes.getValue(Name.SPECIFICATION_TITLE);
				specVendor  = sectionAttributes.getValue(Name.SPECIFICATION_VENDOR);
				specVersion = sectionAttributes.getValue(Name.SPECIFICATION_VERSION);
				implTitle   = sectionAttributes.getValue(Name.IMPLEMENTATION_TITLE);
				implVendor  = sectionAttributes.getValue(Name.IMPLEMENTATION_VENDOR);
				implVersion = sectionAttributes.getValue(Name.IMPLEMENTATION_VERSION);
				sealed      = sectionAttributes.getValue(Name.SEALED);
			}

			if (mainAttributes != null)
			{
				if (specTitle == null)
					specTitle = mainAttributes.getValue(Name.SPECIFICATION_TITLE);

				if (specVendor == null)
					specVendor = mainAttributes.getValue(Name.SPECIFICATION_VENDOR);

				if (specVersion == null)
					specVersion = mainAttributes.getValue(Name.SPECIFICATION_VERSION);

				if (implTitle == null)
					implTitle = mainAttributes.getValue(Name.IMPLEMENTATION_TITLE);

				if (implVendor == null)
					implVendor = mainAttributes.getValue(Name.IMPLEMENTATION_VENDOR);
            
				if (implVersion == null) 
					implVersion = mainAttributes.getValue(Name.IMPLEMENTATION_VERSION);
            
				if (sealed == null) 
					sealed = mainAttributes.getValue(Name.SEALED);
            		}
		}

		/* If the package was defined as sealed then use the jar url for the base */
		if (sealed != null && sealed.equalsIgnoreCase("true"))
			sealBase = m_url;

		/* finally define the package and return */
		definePackage(packageName, 
					  specTitle, specVersion, specVendor, 
					  implTitle, implVersion, implVendor, 
					  sealBase);
	}

}
