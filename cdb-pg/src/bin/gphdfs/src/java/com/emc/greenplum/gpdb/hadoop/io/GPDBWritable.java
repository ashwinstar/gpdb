package com.emc.greenplum.gpdb.hadoop.io;

import java.io.IOException;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInput;
import java.io.DataOutput;
import java.io.DataOutputStream;
import java.io.DataInputStream;

import org.apache.hadoop.io.Writable;


/**
 * This class represents a GPDB record in the form of
 * a Java object.
 * @author achoi
 *
 */
public class GPDBWritable implements Writable {

	/**
	 * "DEFINE" of the Database Datatype OID
	 * All datatypes are supported. The types listed here
	 * are commonly used type to facilitaes ease-of-use.
	 * Note that String should always be UTF-8 formatted.
	 */
	public static final int BOOLEAN   =   16;
	public static final int BYTEA     =   17;
	public static final int CHAR      =   18;
	public static final int BIGINT    =   20;
	public static final int SMALLINT  =   21;
	public static final int INTEGER   =   23;
	public static final int TEXT      =   25;
	public static final int REAL      =  700;
	public static final int FLOAT8    =  701;
	public static final int BPCHAR    = 1042;
	public static final int VARCHAR   = 1043;
	public static final int DATE      = 1082;
	public static final int TIME      = 1083;
	public static final int TIMESTAMP = 1114;
	public static final int NUMERIC   = 1700;
	
	/**
	 * Constants
	 */
	private static final int    VERSION = 1;
	private static final String CHARSET = "UTF-8";
	private static final int    RECHEADERLEN = 12; /* length, version and col cnt */
	private static final int    COLHEADERLEN = 9;  /* coltype, collen, bitflag(1) */
	
	/**
	 * Local variables
	 */
	protected int[]    colType;
	protected Object[] colValue;
	
	/**
	 * An exception class for column type definition and
	 * set/get value mismatch.
	 * @author achoi
	 *
	 */
	public class TypeMismatchException extends IOException {
		public TypeMismatchException(String msg) {
			super(msg);
		}
	}

	/**
	 * Empty Constructor
	 */
	public GPDBWritable() {
	}
	
	/**
	 * Constructor to build a db record. colType defines the schema
	 * @param colType the table column types
	 */
	public GPDBWritable(int[] columnType) {
		colType = columnType;
		colValue = new Object[columnType.length];
	}
	
	/**
	 * Constructor to build a db record from a serialized form.
	 * @param data a record in the serialized form
	 * @throws IOException if the data is malformatted.
	 */
	public GPDBWritable(byte[] data) throws IOException {
		ByteArrayInputStream bis = new ByteArrayInputStream(data);
		DataInputStream      dis = new DataInputStream(bis);

		this.readFields(dis);
	}
	
	/**
	 * Serialize the object using the following format:
	 * Total Length | Version | Number of columns | Col type |	Bit flag | Col len ...| Col val
     * 4 byte		| 4 byte	4 byte				4 byte	    1 byte	   4 byte		<var len>
	 */

	/**
	 *  @inheritDoc
	 */
	public void readFields(DataInput in) throws IOException {
		/* extract the version and col cnt */
		int pklen   = in.readInt();
		int version = in.readInt();
		int colCnt  = in.readInt();
		
		/* !!! Check VERSION !!! */
		if (version != this.VERSION) {
			throw new IOException("Current GPDBWritable version(" +
					this.VERSION + ") does not match input version(" +
					version+")");
		}
		
		/* extract column definition */
		colType        = new int[colCnt];
		colValue       = new Object[colCnt];
		byte[] bitflag = new byte[colCnt];
		int[] colLen   = new int[colCnt];
		for(int i=0; i<colCnt; i++) {
			colType[i] = in.readInt();
			bitflag[i] = in.readByte();
			colLen[i]  = in.readInt();
		}
		
		/* extract column value */
		for(int i=0; i<colCnt; i++) {
			if (bitflag[i] != 1) {
				switch(colType[i]) {
					case BIGINT:   { long    val = in.readLong();    colValue[i] = new Long(val);    break; } 
					case BOOLEAN:  { boolean val = in.readBoolean(); colValue[i] = new Boolean(val); break; }
					case FLOAT8:   { double  val = in.readDouble();  colValue[i] = new Double(val);  break; }
					case INTEGER:  { int     val = in.readInt();     colValue[i] = new Integer(val); break; }
					case REAL:     { float   val = in.readFloat();   colValue[i] = new Float(val);   break; }
					case SMALLINT: { short   val = in.readShort();   colValue[i] = new Short(val);   break; }
					case BYTEA:    { 
						colValue[i] = new byte[colLen[i]]; 
						in.readFully((byte[])colValue[i], 0, colLen[i]);
						break;
					}
					/* For text formatted column, it's always null terminated string.
					 * So, we can remove the last "\0" when constructing the string.
					 */
					default:       {
						byte[] data = new byte[colLen[i]];
						in.readFully(data, 0, colLen[i]);
						colValue[i] = new String(data, 0, colLen[i]-1, CHARSET);
					}
				}
			}
		}
	}
	
	
	/**
	 *  @inheritDoc
	 */
	public void write(DataOutput out) throws IOException {
		/* We need to know the bit flag list and col length list first */
		int    numCol    = colType.length;
		byte[] bitflags  = new byte[numCol];
		int[]  colLength = new int[numCol];
		
		for(int i=0; i<numCol; i++) {
			if (colValue[i] == null) {
				bitflags[i]  = 1;
				colLength[i] = 0;
			} else {
				bitflags[i] = 0;
				switch(colType[i]) {
					case BIGINT:   colLength[i] = 8; break;
					case BOOLEAN:  colLength[i] = 1; break;
					case FLOAT8:   colLength[i] = 8; break;
					case INTEGER:  colLength[i] = 4; break;
					case REAL:     colLength[i] = 4; break;
					case SMALLINT: colLength[i] = 2; break;
					case BYTEA:    colLength[i] = ((byte[])colValue[i]).length; break;
					/* For text format, add a "\0" at the end */
					default: colLength[i] = ((String)colValue[i]).getBytes(CHARSET).length + 1;
				}
			}
		}
		
		/* Now, compute the total payload and header length */
		int packetlen = RECHEADERLEN;
		for(int i=0; i<numCol; i++)
			packetlen += COLHEADERLEN + colLength[i];

		/* Construct the packet header */
		out.writeInt(packetlen);
		out.writeInt(VERSION);
		out.writeInt(numCol);
		
		/* Column definitions */
		for(int i=0; i<numCol; i++) {
			out.writeInt(colType[i]);
			out.writeByte(bitflags[i]);
			out.writeInt(colLength[i]);
		}
		
		/* Column Value */
		for(int i=0; i<numCol; i++) {
			if (bitflags[i] == 0) {
				switch(colType[i]) {
					case BIGINT:   out.writeLong(   ((Long)   colValue[i]).longValue());    break;
					case BOOLEAN:  out.writeBoolean(((Boolean)colValue[i]).booleanValue()); break;
					case FLOAT8:   out.writeDouble( ((Double) colValue[i]).doubleValue());  break;
					case INTEGER:  out.writeInt(    ((Integer)colValue[i]).intValue());     break;
					case REAL:     out.writeFloat(  ((Float)  colValue[i]).floatValue());   break;
					case SMALLINT: out.writeShort(  ((Short)  colValue[i]).shortValue());   break;
					case BYTEA:    out.write((byte[])colValue[i], 0, colLength[i]);         break;
					/* For text format, add a "\0" at the end */
					default: {
						byte[] data = ((String)colValue[i]+"\0").getBytes(CHARSET);
						out.write(data, 0, colLength[i]);
						break;
					}
				}	
			}
		}
	}
		
	/**
	 * Return the byte form
	 * @return the binary representation of this object
	 * @throws if an I/O error occured
	 */
	public byte[] toBytes() throws IOException {
		ByteArrayOutputStream baos = new ByteArrayOutputStream(256);
		DataOutputStream      dos  = new DataOutputStream(baos);
		
		this.write(dos);
		
		return baos.toByteArray();
	}
	
	/**
	 * Getter/Setter methods to get/set the column value
	 */
	
	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setLong(int colIdx, Long val)
	throws TypeMismatchException {
		checkType(BIGINT, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setBoolean(int colIdx, Boolean val)
	throws TypeMismatchException {
		checkType(BOOLEAN, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setBytes(int colIdx, byte[] val)
	throws TypeMismatchException {
		checkType(BYTEA, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setString(int colIdx, String val)
	throws TypeMismatchException {
		checkType(TEXT, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setFloat(int colIdx, Float val)
	throws TypeMismatchException {
		checkType(REAL, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setDouble(int colIdx, Double val)
	throws TypeMismatchException {
		checkType(FLOAT8, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setInt(int colIdx, Integer val)
	throws TypeMismatchException {
		checkType(INTEGER, colIdx, true);
		colValue[colIdx] = val;
	}

	/**
	 * Set the column value of the record
	 * @param colIdx the column index
	 * @param val the value
	 * @throw TypeMismatchException the column type does not match
	 */
	public void setShort(int colIdx, Short val)
	throws TypeMismatchException {
		checkType(SMALLINT, colIdx, true);
		colValue[colIdx] = new Short(val);
	}
	
	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Long getLong(int colIdx)
	throws TypeMismatchException {
		checkType(BIGINT, colIdx, false);
		return (Long)colValue[colIdx];
	}

	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Boolean getBoolean(int colIdx)
	throws TypeMismatchException {
		checkType(BOOLEAN, colIdx, false);
		return (Boolean)colValue[colIdx];
	}

	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public byte[] getBytes(int colIdx)
	throws TypeMismatchException {
		checkType(BYTEA, colIdx, false);
		return (byte[])colValue[colIdx];
	}

	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public String getString(int colIdx)
	throws TypeMismatchException {
		checkType(TEXT, colIdx, false);
		return (String)colValue[colIdx];
	}

	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Float getFloat(int colIdx)
	throws TypeMismatchException {
		checkType(REAL, colIdx, false);
		return (Float)colValue[colIdx];
	}

	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Double getDouble(int colIdx)
	throws TypeMismatchException {
		checkType(FLOAT8, colIdx, false);
		return (Double)colValue[colIdx];
	}
	
	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Integer getInt(int colIdx)
	throws TypeMismatchException {
		checkType(INTEGER, colIdx, false);
		return (Integer)colValue[colIdx];
	}
	
	/**
	 * Get the column value of the record
	 * @param colIdx the column index
	 * @throw TypeMismatchException the column type does not match
	 */
	public Short getShort(int colIdx)
	throws TypeMismatchException {
		checkType(SMALLINT, colIdx, false);
		return (Short)colValue[colIdx];
	}
	
	/**
	 * @inheritDoc
	 */
	public String toString() {
		if (colType == null)
			return null;
		
		String result= "";
		for(int i=0; i<colType.length; i++) {
			result += "Column " + i + ":";
			if (colValue[i] != null) {
				if (colType[i] == BYTEA)
					result += byteArrayInString((byte[])colValue[i]);
				else
					result += colValue[i].toString();
			}
			result += "\n";
		}		
		return result;
	}
	
	/**
	 * Helper printing function
	 */
	private static String byteArrayInString(byte[] data) {
		String result = "";
		for(int i=0; i<data.length; i++) {
			Byte b = new Byte(data[i]);
			result += b.intValue() + " ";
		}
		return result;
	}

	
	/**
	 * Private Helper to check the type mismatch
	 * If the expected type is stored as string, then it must be set
	 * via setString.
	 * Otherwise, the type must match.
	 */
	private void checkType(int inTyp, int idx, boolean isSet)
	throws TypeMismatchException {
		if (idx < 0 || idx >= colType.length)
			throw new TypeMismatchException(
					"Column index is out of range");
		
		int exTyp = colType[idx];
		String errmsg;

		if (isTextForm(exTyp)) {
			if (inTyp != TEXT)
				throw new TypeMismatchException(formErrorMsg(inTyp, TEXT, isSet));
		} else if (inTyp != exTyp)
			throw new TypeMismatchException(formErrorMsg(inTyp, exTyp, isSet));
	}
	
	private String formErrorMsg(int inTyp, int colTyp, boolean isSet) {
		String errmsg;
		if (isSet)
			errmsg = "Cannot set "+getTypeName(inTyp) + " to a " +
				getTypeName(colTyp) + " column";
		else
			errmsg = "Cannot get "+getTypeName(inTyp) + " from a " +
				getTypeName(colTyp) + " column";
		return errmsg;
	}

	/**
	 * Private Helper routine to tell whether a type is Text form or not
	 * @param type the type OID that we want to check
	 */
	private boolean isTextForm(int type) {
		if (type==BIGINT ||
			type==BOOLEAN ||
			type==BYTEA ||
			type==FLOAT8 ||
			type==INTEGER ||
			type==REAL ||
			type==SMALLINT)
			return false;
		return true;
	}
	
	/**
	 * Private Helper to get the type name for rasing error.
	 * If a given oid is not in the commonly used list, we
	 * would expect a TEXT for it (for the error message).
	 */
	private static String getTypeName(int oid) {
		switch(oid) {
		case BOOLEAN  : return "BOOLEAN";
		case BYTEA    : return "BYTEA";
		case CHAR     : return "CHAR";
		case BIGINT   : return "BIGINT";
		case SMALLINT : return "SMALLINT";
		case INTEGER  : return "INTEGER";
		case TEXT     : return "TEXT";
		case REAL     : return "REAL";
		case FLOAT8   : return "FLOAT8";
		case BPCHAR   : return "BPCHAR";
		case VARCHAR  : return "VARCHAR";
		case DATE     : return "DATE";
		case TIME     : return "TIME";
		case TIMESTAMP: return "TIMESTAMP";
		case NUMERIC  : return "NUMERIC";
		default:        return "TEXT";
		}
	}

}
